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

import struct
import uuid


class PebbleAppHeader(object):
    MAGIC = 'PBLAPP\x00\x00'

    # 10 bytes
    HEADER_STRUCT_DEFINITION = [
        '8s',   # header
        '2B',   # struct version
    ]
    HEADER_STRUCT = struct.Struct(''.join(HEADER_STRUCT_DEFINITION))

    # 116 bytes
    V1_STRUCT_VERSION = (0x08, 0x01)
    V1_STRUCT_DEFINITION = [
        # format, name, deserialization transform, serialization transform
        ('B', 'sdk_version_major', None, None),
        ('B', 'sdk_version_minor', None, None),
        ('B', 'app_version_major', None, None),
        ('B', 'app_version_minor', None, None),
        ('H', 'app_size', None, None),
        ('I', 'offset', None, None),
        ('I', 'crc', None, None),
        ('32s', 'app_name', lambda bytes: bytes.rstrip('\0'), None),
        ('32s', 'company_name', lambda bytes: bytes.rstrip('\0'), None),
        ('I', 'icon_resource_id', None, None),
        ('I', 'symbol_table_addr', None, None),
        ('I', 'flags', None, None),
        ('I', 'relocation_list_index', None, None),
        ('I', 'num_relocation_entries', None, None),
        ('16s', 'uuid', lambda s: uuid.UUID(bytes=s), lambda u: u.bytes),
    ]

    # 120 bytes
    V2_STRUCT_VERSION = (0x10, 0x00)
    V2_STRUCT_DEFINITION = list(V1_STRUCT_DEFINITION)
    del V2_STRUCT_DEFINITION[12]  # relocation list was dropped in v2.x
    V2_STRUCT_DEFINITION += [
        ('I', 'resource_crc', None, None),
        ('I', 'resource_timestamp', None, None),
        ('H', 'virtual_size', None, None),
    ]
    V2_HEADER_LENGTH = 10 + 120

    DEFINITION_MAP = {
        V1_STRUCT_VERSION: V1_STRUCT_DEFINITION,
        V2_STRUCT_VERSION: V2_STRUCT_DEFINITION,
    }

    @classmethod
    def get_def_and_struct(cls, struct_version):
        definition = cls.DEFINITION_MAP.get(struct_version)
        if not definition:
            raise Exception("Unsupported "
                            "struct version %s" % str(struct_version))
        fmt = '<' + reduce(lambda s, t: s + t[0], definition, '')
        s = struct.Struct(fmt)
        return definition, s

    @classmethod
    def deserialize(cls, app_bin):
        header_size = cls.HEADER_STRUCT.size
        header = app_bin[0:header_size]
        values = cls.HEADER_STRUCT.unpack(header)
        struct_version = (values[1], values[2])
        info = {
            'sentinel': values[0],
            'struct_version_major': struct_version[0],
            'struct_version_minor': struct_version[1],
        }

        if info['sentinel'] != cls.MAGIC:
            raise Exception('This is not a pebble watchapp')

        definition, s = cls.get_def_and_struct(struct_version)
        values = s.unpack(app_bin[header_size:header_size + s.size])
        for value, elem in zip(values, definition):
            field_name = elem[1]
            transform = elem[2]
            info[field_name] = value if not transform else transform(value)
        return info

    def serialize(self):
        struct_version = (self._info['struct_version_major'],
                          self._info['struct_version_minor'])
        header = PebbleAppHeader.HEADER_STRUCT.pack(PebbleAppHeader.MAGIC,
                                                    *struct_version)

        definition, s = self.__class__.get_def_and_struct(struct_version)

        def map_args(elem):
            value = self._info[elem[1]]
            transform = elem[3]
            return value if not transform else transform(value)
        args = map(map_args, definition)

        return header + s.pack(*args)

    def __init__(self, app_bin_bytes):
        self._info = PebbleAppHeader.deserialize(app_bin_bytes)

    def __getattr__(self, name):
        value = self._info.get(name)
        if value is None:
            raise Exception("Unknown field %s" % name)
        return value

    def __setattr__(self, name, value):
        if name == '_info':
            super(PebbleAppHeader, self).__setattr__(name, value)
        self._info[name] = value

    def __repr__(self):
        return self._info.__repr__()

    def __str__(self):
        return self.__repr__()


if __name__ == '__main__':
    import argparse
    import pprint
    parser = argparse.ArgumentParser()
    parser.add_argument('bin_file')
    args = parser.parse_args()

    with open(args.bin_file, 'rb') as f:
        app_info = PebbleAppHeader.deserialize(f.read())

    pprint.pprint(app_info)
