#!/usr/bin/env python
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


from binascii import crc32
import os
import struct

import stm32_crc


class PebbleFirmwareBinaryInfo(object):
    V1_STRUCT_VERSION = 1
    V1_STRUCT_DEFINITION = [
        ('20s', 'build_id'),
        ('L', 'version_timestamp'),
        ('32s', 'version_tag'),
        ('8s', 'version_short'),
        ('?', 'is_recovery_firmware'),
        ('B', 'hw_platform'),
        ('B', 'metadata_version')
    ]
    # The platforms which use a legacy defective crc32
    LEGACY_CRC_PLATFORMS = [
        0,  # unknown (assume legacy)
        1,  # OneEV1
        2,  # OneEV2
        3,  # OneEV2_3
        4,  # OneEV2_4
        5,  # OnePointFive
        6,  # TwoPointFive
        7,  # SnowyEVT2
        8,  # SnowyDVT
        9,  # SpaldingEVT
        10,  # BobbyDVT
        11,  # Spalding
        0xff,  # OneBigboard
        0xfe,  # OneBigboard2
        0xfd,  # SnowyBigboard
        0xfc,  # SnowyBigboard2
        0xfb,  # SpaldingBigboard
    ]

    def get_crc(self):
        _, ext = os.path.splitext(self.path)
        assert ext == '.bin', 'Can only calculate crc for .bin files'
        with open(self.path, 'rb') as f:
            image = f.read()
        if self.hw_platform in self.LEGACY_CRC_PLATFORMS:
            # use the legacy defective crc
            return stm32_crc.crc32(image)
        else:
            # use a regular crc
            return crc32(image) & 0xFFFFFFFF

    def _get_footer_struct(self):
        fmt = '<' + reduce(lambda s, t: s + t[0],
                           PebbleFirmwareBinaryInfo.V1_STRUCT_DEFINITION, '')
        return struct.Struct(fmt)

    def _get_footer_data_from_elf(self, path):
        import binutils
        fw_version_data = binutils.section_bytes(path, '.fw_version')
        # The GNU Build ID has 16 bytes of header data, strip it off:
        build_id_data = binutils.section_bytes(path, '.note.gnu.build-id')[16:]
        return build_id_data + fw_version_data

    def _get_footer_data_from_bin(self, path):
        with open(path, 'rb') as f:
            struct_size = self.struct.size
            f.seek(-struct_size, 2)
            footer_data = f.read()
            return footer_data

    def _parse_footer_data(self, footer_data):
        z = zip(PebbleFirmwareBinaryInfo.V1_STRUCT_DEFINITION,
                self.struct.unpack(footer_data))
        return {entry[1]: data for entry, data in z}

    def __init__(self, elf_or_bin_path):
        self.path = elf_or_bin_path
        self.struct = self._get_footer_struct()
        _, ext = os.path.splitext(elf_or_bin_path)
        if ext == '.elf':
            footer_data = self._get_footer_data_from_elf(elf_or_bin_path)
        elif ext == '.bin':
            footer_data = self._get_footer_data_from_bin(elf_or_bin_path)
        else:
            raise ValueError('Unexpected extension. Must be ".bin" or ".elf"')
        self.info = self._parse_footer_data(footer_data)

        # Trim leading NULLS on the strings:
        for k in ["version_tag", "version_short"]:
            self.info[k] = self.info[k].rstrip("\x00")

    def __str__(self):
        return str(self.info)

    def __repr__(self):
        return self.info.__repr__()

    def __getattr__(self, name):
        if name in self.info:
            return self.info[name]
        raise AttributeError


if __name__ == "__main__":
    import argparse
    import pprint
    parser = argparse.ArgumentParser()
    parser.add_argument('fw_bin_or_elf_path')
    args = parser.parse_args()

    fw_bin_info = PebbleFirmwareBinaryInfo(args.fw_bin_or_elf_path)

    pprint.pprint(fw_bin_info)
