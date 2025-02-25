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

from __future__ import print_function

from binascii import crc32
import os
import struct
import sys
import traceback

import pebble.pulse2.exceptions

from .. import PebbleCommander, exceptions, parsers
from ..util import stm32_crc


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

    def __init__(self, bin_path):
        self.path = bin_path
        self.struct = self._get_footer_struct()
        _, ext = os.path.splitext(bin_path)
        if ext != '.bin':
            raise ValueError('Unexpected extension. Must be ".bin"')
        footer_data = self._get_footer_data_from_bin(bin_path)
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


# typedef struct ATTR_PACKED FirmwareDescription {
#   uint32_t description_length;
#   uint32_t firmware_length;
#   uint32_t checksum;
# } FirmwareDescription;
FW_DESCR_FORMAT = '<III'
FW_DESCR_SIZE = struct.calcsize(FW_DESCR_FORMAT)


def _generate_firmware_description_struct(firmware_length, firmware_crc):
    return struct.pack(FW_DESCR_FORMAT, FW_DESCR_SIZE, firmware_length, firmware_crc)


def insert_firmware_description_struct(input_binary, output_binary=None):
    fw_bin_info = PebbleFirmwareBinaryInfo(input_binary)
    with open(input_binary, 'rb') as inf:
        fw_bin = inf.read()
        fw_crc = fw_bin_info.get_crc()
    return _generate_firmware_description_struct(len(fw_bin), fw_crc) + fw_bin


def _load(connection, image, progress, verbose, address):
    image_crc = stm32_crc.crc32(image)

    progress_cb = None
    if progress or verbose:
        def progress_cb(acked):
            print('.' if acked else 'R', end='')
            sys.stdout.flush()

    if progress or verbose:
        print('Erasing... ', end='')
        sys.stdout.flush()
    try:
        connection.flash.erase(address, len(image))
    except pebble.pulse2.exceptions.PulseException as e:
        detail = ''.join(traceback.format_exception_only(type(e), e))
        if verbose:
            detail = '\n' + traceback.format_exc()
        print('Erase failed! ' + detail)
        return False
    if progress or verbose:
        print('done.')
        sys.stdout.flush()

    try:
        retries = connection.flash.write(address, image,
                                         progress_cb=progress_cb)
    except pebble.pulse2.exceptions.PulseException as e:
        detail = ''.join(traceback.format_exception_only(type(e), e))
        if verbose:
            detail = '\n' + traceback.format_exc()
        print('Write failed! ' + detail)
        return False

    result_crc = connection.flash.crc(address, len(image))

    if progress or verbose:
        print()
    if verbose:
        print('Retries: %d' % retries)

    return result_crc == image_crc


def load_firmware(connection, fin, progress, verbose, address=None):
    if address is None:
        # If address is unspecified, assume we want the prf address
        _, address, length = connection.flash.query_region_geometry(
                connection.flash.REGION_PRF)
    address = int(address)

    image = insert_firmware_description_struct(fin)
    if _load(connection, image, progress, verbose, address):
        connection.flash.finalize_region(
            connection.flash.REGION_PRF)
        return True
    return False


def load_resources(connection, fin, progress, verbose):
    _, address, length = connection.flash.query_region_geometry(
            connection.flash.REGION_SYSTEM_RESOURCES)

    with open(fin, 'rb') as f:
        data = f.read()
    assert len(data) <= length
    if _load(connection, data, progress, verbose, address):
        connection.flash.finalize_region(
                connection.flash.REGION_SYSTEM_RESOURCES)
        return True
    return False


@PebbleCommander.command()
def image_resources(cmdr, pack='build/system_resources.pbpack'):
    """ Image resources.
    """
    load_resources(cmdr.connection, pack,
                   progress=cmdr.interactive, verbose=cmdr.interactive)


@PebbleCommander.command()
def image_firmware(cmdr, firm='build/prf/src/fw/tintin_fw.bin', address=None):
    """ Image recovery firmware.
    """
    if address is not None:
        address = int(str(address), 0)
    load_firmware(cmdr.connection, firm, progress=cmdr.interactive,
                  verbose=cmdr.interactive, address=address)
