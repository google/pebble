# Copyright 2024 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import argparse
import json
import os
import sh
import string


class ApplibType(object):
    def __init__(self, type_dict):
        self.name = type_dict['name']

        self.check_size = 1  # C preproc bool: 1 = true, 0 = false
        self.min_sdk = 0
        self.size_2x = type_dict.get('size_2x', 0)
        self.size_3x_direct_padding = type_dict.get('size_3x_padding', 0)
        self.size_3x = type_dict.get('size_3x', 0)

        self.dependencies = type_dict.get('dependencies', [])

        self.total_3x_padding = None

    def get_total_3x_padding(self, all_types):
        """ Return the amount of padding to use for the 3x version of the struct including both
            the direct padding we add for this struct in particular as well as all padding needed
            for all dependent structs.
        """

        if self.total_3x_padding is not None:
            # We have it cached, just return the previously calculated value
            return self.total_3x_padding

        self.total_3x_padding = self.size_3x_direct_padding

        for d in self.dependencies:
            parent = filter(lambda t: d == t.name, all_types)[0]
            self.total_3x_padding += parent.get_total_3x_padding(all_types)

        return self.total_3x_padding

    def __repr__(self):
        return '<%s %s>' % (self.__class__.__name__, self.name)


def get_types(data):
    return [ApplibType(t) for t in data['types'] if 'name' in t]


def writeline(f, str=''):
    f.write(str + '\n')


def write_template(f, filepath, replace):
    with open(filepath, 'r') as template_file:
        template = string.Template(template_file.read())
        f.write(template.safe_substitute(**replace) + '\n')


def generate_header(data, output_filename):
    all_types = get_types(data)

    with open(output_filename, 'w') as f:
        write_template(f, 'tools/applib_malloc.template.h', {
            'filename': output_filename,
        })

        for t in all_types:
            write_template(f, 'tools/applib_malloc_type.template.h', t.__dict__)


def generate_implementation(data, output_filename, min_sdk, disable_size_checks=False):
    all_types = get_types(data)
    with open(output_filename, 'w') as f:
        includes = ['#include "%s"' % h for h in data['headers']]
        applib_enum_types = ['ApplibType_%s' % t.name for t in all_types]
        applib_malloc_types = ['{ sizeof(%s), %u, %u }' % (t.name, t.size_2x, t.size_3x)
                               for t in all_types]

        write_template(f, 'tools/applib_malloc.template.c', {
            'filename': os.path.basename(output_filename),
            'includes': '\n'.join(includes),
            'applib_enum_types': ',\n  '.join(applib_enum_types),
            'applib_malloc_types': ',\n  '.join(applib_malloc_types),
        })

        for t in all_types:
            t.min_sdk = min_sdk
            t.check_size = 0 if disable_size_checks else 1
            t.get_total_3x_padding(all_types)  # Populate the value
            write_template(f, 'tools/applib_malloc_type.template.c', t.__dict__)


def generate_files(json_filename, header_filename, impl_filename, min_sdk,
                   disable_size_checks=False):
    with open(json_filename) as f:
        data = json.load(f)

    generate_header(data, header_filename)
    generate_implementation(data, impl_filename, min_sdk, disable_size_checks)


def _get_sizeof_type(elf_filename, typename):
    def _run_gdb(cmd):
        running_cmd = sh.arm_none_eabi_gdb(elf_filename, batch=True, nx=True, ex=cmd)
        result = str(running_cmd)

        # Strip escape sequences if present
        if result[0] == '\x1b':
            result = result[8:]

        return result.strip()

    gdb_output = _run_gdb('p sizeof(%s)' % typename)

    if len(gdb_output) == 0:
        # Sometimes gdb is dumb and fails at interpreting a typedef, try again with a struct prefix
        gdb_output = _run_gdb('p sizeof(struct %s)' % typename)

    if len(gdb_output) == 0:
        raise Exception("Failed to get sizeof for type %s" % typename)

    # Looks like "$1 = 44", we want the "44"
    return int(gdb_output.split()[2])


def dump_sizes(json_filename, elf_filename):
    with open(json_filename) as f:
        data = json.load(f)

    all_types = get_types(data)
    fmt_str = "%30s %10s %10s %10s %16s %16s %16s  %s"

    print fmt_str % ("Type", "sizeof()", "Size 2.x", "Size 3.x",
                     "direct padding", "total padding", "calculated size", "dependencies")

    for t in all_types:
        type_sizeof = _get_sizeof_type(elf_filename, t.name)

        calculated_size = type_sizeof + t.get_total_3x_padding(all_types)
        if not t.size_3x or calculated_size == t.size_3x:
            calculated_size_str = str(calculated_size)
        else:
            calculated_size_str = "%u <%u>" % (calculated_size, (calculated_size - t.size_3x))

        print fmt_str % (t.name, type_sizeof, t.size_2x, t.size_3x,
                         t.size_3x_direct_padding, t.get_total_3x_padding(all_types),
                         calculated_size_str, t.dependencies)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('--json', type=str, default='src/fw/applib/applib_malloc.json',
                        help="Specify the JSON file to use")
    parser.add_argument('--elf', type=str, default='build/src/fw/tintin_fw.elf',
                        help="Specify the ELF file to use")

    args = parser.parse_args()
    dump_sizes(args.json, args.elf)
