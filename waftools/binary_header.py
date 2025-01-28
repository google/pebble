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

import binascii

import sparse_length_encoding

from waflib import Task, TaskGen, Utils, Node, Errors

class binary_header(Task.Task):
    """
    Create a header file containing an array with contents from a binary file.
    """

    def run(self):
        if getattr(self.generator, 'hex', False):
            # Input file is hexadecimal ASCII characters with whitespace
            text = self.inputs[0].read(
                    encoding=getattr(self.generator, 'encoding', 'ISO8859-1'))
            # Strip all whitespace so that binascii is happy
            text = ''.join(text.split())
            code = binascii.unhexlify(text)
        else:
            code = self.inputs[0].read('rb')

        array_name = getattr(self.generator, 'array_name', None)
        if not array_name:
            array_name = re.sub(r'[^A-Za-z0-9]', '_', self.inputs[0].name)

        if getattr(self.generator, 'compressed', False):
            encoded_code = ''.join(sparse_length_encoding.encode(code))
            # verify that it was encoded correctly
            if ''.join(sparse_length_encoding.decode(encoded_code)) != code:
                raise Errors.WafError('encoding error')
            code = encoded_code

        output = ['#pragma once', '#include <stdint.h>']
        output += ['static const uint8_t %s[] = {' % array_name]
        line = []
        for n, b in enumerate(code):
            line += ['0x%.2x,' % ord(b)]
            if n % 16 == 15:
                output += [''.join(line)]
                line = []
        if line:
            output += [''.join(line)]
        output += ['};', '']

        self.outputs[0].write(
                '\n'.join(output),
                encoding=getattr(self.generator, 'encoding', 'ISO8859-1'))
        self.generator.bld.raw_deps[self.uid()] = self.dep_vars = 'array_name'

        if getattr(self.generator, 'chmod', None):
            os.chmod(self.outputs[0].abspath(), self.generator.chmod)

    def sig_vars(self):
        dependent_generator_vars = ['hex', 'encoding', 'array_name',
                                    'compressed', 'chmod']
        vars = []
        for k in dependent_generator_vars:
            try:
                vars.append((k, getattr(self.generator, k)))
            except AttributeError:
                pass
        self.m.update(Utils.h_list(vars))
        return self.m.digest()


@TaskGen.feature('binary_header')
@TaskGen.before_method('process_source', 'process_rule')
def process_binary_header(self):
    """
    Define a transformation that substitutes the contents of *source* files to
    *target* files::

        def build(bld):
            bld(
                features='binary_header',
                source='foo.bin',
                target='foo.auto.h',
                array_name='s_some_array',
                compressed=True
            )
            bld(
                features='binary_header',
                source='bar.hex',
                target='bar.auto.h',
                hex=True
            )

    If the *hex* parameter is True, the *source* files are read in an ASCII
    hexadecimal format, where each byte is represented by a pair of hexadecimal
    digits with optional whitespace. If *hex* is False or not specified, the
    file is treated as a raw binary file.

    If the *compressed* parameter is True, the *source* files are compressed with
    sparse length encoding (see waftools/sparse_length_encoding.py).

    The name of the array variable defaults to the source file name with all
    characters that are invalid C identifiers replaced with underscores. The name
    can be explicitly specified by setting the *array_name* parameter.

    This method overrides the processing by
    :py:meth:`waflib.TaskGen.process_source`.
    """

    src = Utils.to_list(getattr(self, 'source', []))
    if isinstance(src, Node.Node):
        src = [src]
    tgt = Utils.to_list(getattr(self, 'target', []))
    if isinstance(tgt, Node.Node):
        tgt = [tgt]
    if len(src) != len(tgt):
        raise Errors.WafError('invalid number of source/target for %r' % self)

    for x, y in zip(src, tgt):
        if not x or not y:
            raise Errors.WafError('null source or target for %r' % self)
        a, b = None, None

        if isinstance(x, str) and isinstance(y, str) and x == y:
            a = self.path.find_node(x)
            b = self.path.get_bld().make_node(y)
            if not os.path.isfile(b.abspath()):
                b.sig = None
                b.parent.mkdir()
        else:
            if isinstance(x, str):
                a = self.path.find_resource(x)
            elif isinstance(x, Node.Node):
                a = x
            if isinstance(y, str):
                b = self.path.find_or_declare(y)
            elif isinstance(y, Node.Node):
                b = y

        if not a:
            raise Errors.WafError('could not find %r for %r' % (x, self))

        has_constraints = False
        tsk = self.create_task('binary_header', a, b)
        for k in ('after', 'before', 'ext_in', 'ext_out'):
            val = getattr(self, k, None)
            if val:
                has_constraints = True
                setattr(tsk, k, val)

        tsk.before = [k for k in ('c', 'cxx') if k in Task.classes]

    self.source = []

