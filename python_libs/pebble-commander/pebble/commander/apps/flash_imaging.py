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

from __future__ import absolute_import

import collections
import struct
import time

import pebble.pulse2.exceptions

from .. import exceptions


class EraseCommand(object):

    command_type = 1
    command_struct = struct.Struct('<BII')

    response_type = 128
    response_struct = struct.Struct('<xII?')
    Response = collections.namedtuple(
            'EraseResponse', 'address length complete')

    def __init__(self, address, length):
        self.address = address
        self.length = length

    @property
    def packet(self):
        return self.command_struct.pack(
                self.command_type, self.address, self.length)

    def parse_response(self, response):
        if ord(response[0]) != self.response_type:
            raise exceptions.ResponseParseError(
                    'Unexpected response: %r' % response)
        unpacked = self.Response._make(self.response_struct.unpack(response))
        if unpacked.address != self.address or unpacked.length != self.length:
            raise exceptions.ResponseParseError(
                'Response does not match command: '
                'address=%#.08x length=%d (expected %#.08x, %d)' % (
                    unpacked.address, unpacked.length, self.address,
                    self.length))
        return unpacked


class WriteCommand(object):

    command_type = 2
    command_struct = struct.Struct('<BI')
    header_len = command_struct.size

    def __init__(self, address, data):
        self.address = address
        self.data = data

    @property
    def packet(self):
        header = self.command_struct.pack(self.command_type, self.address)
        return header + self.data


class WriteResponse(object):

    response_type = 129
    response_struct = struct.Struct('<xII?')
    Response = collections.namedtuple(
            'WriteResponse', 'address length complete')

    @classmethod
    def parse(cls, response):
        if ord(response[0]) != cls.response_type:
            raise exceptions.ResponseParseError(
                    'Unexpected response: %r' % response)
        return cls.Response._make(cls.response_struct.unpack(response))


class CrcCommand(object):

    command_type = 3
    command_struct = struct.Struct('<BII')

    response_type = 130
    response_struct = struct.Struct('<xIII')
    Response = collections.namedtuple('CrcResponse', 'address length crc')

    def __init__(self, address, length):
        self.address = address
        self.length = length

    @property
    def packet(self):
        return self.command_struct.pack(self.command_type, self.address,
                                        self.length)

    def parse_response(self, response):
        if ord(response[0]) != self.response_type:
            raise exceptions.ResponseParseError(
                    'Unexpected response: %r' % response)
        unpacked = self.Response._make(self.response_struct.unpack(response))
        if unpacked.address != self.address or unpacked.length != self.length:
            raise exceptions.ResponseParseError(
                    'Response does not match command: '
                    'address=%#.08x length=%d (expected %#.08x, %d)' % (
                        unpacked.address, unpacked.length, self.address,
                        self.length))
        return unpacked


class QueryFlashRegionCommand(object):

    command_type = 4
    command_struct = struct.Struct('<BB')

    REGION_PRF = 1
    REGION_SYSTEM_RESOURCES = 2

    response_type = 131
    response_struct = struct.Struct('<xBII')
    Response = collections.namedtuple(
            'FlashRegionGeometry', 'region address length')

    def __init__(self, region):
        self.region = region

    @property
    def packet(self):
        return self.command_struct.pack(self.command_type, self.region)

    def parse_response(self, response):
        if ord(response[0]) != self.response_type:
            raise exceptions.ResponseParseError(
                    'Unexpected response: %r' % response)
        unpacked = self.Response._make(self.response_struct.unpack(response))
        if unpacked.address == 0 and unpacked.length == 0:
            raise exceptions.RegionDoesNotExist(self.region)
        return unpacked


class FinalizeFlashRegionCommand(object):

    command_type = 5
    command_struct = struct.Struct('<BB')

    response_type = 132
    response_struct = struct.Struct('<xB')

    def __init__(self, region):
        self.region = region

    @property
    def packet(self):
        return self.command_struct.pack(self.command_type, self.region)

    def parse_response(self, response):
        if ord(response[0]) != self.response_type:
            raise exceptions.ResponseParseError(
                    'Unexpected response: %r' % response)
        region, = self.response_struct.unpack(response)
        if region != self.region:
            raise exceptions.ResponseParseError(
                    'Response does not match command: '
                    'response is for region %d (expected %d)' % (
                        region, self.region))


class FlashImaging(object):

    PORT_NUMBER = 0x0002

    RESP_BAD_CMD = 192
    RESP_INTERNAL_ERROR = 193

    REGION_PRF = QueryFlashRegionCommand.REGION_PRF
    REGION_SYSTEM_RESOURCES = QueryFlashRegionCommand.REGION_SYSTEM_RESOURCES

    def __init__(self, link):
        self.socket = link.open_socket('best-effort', self.PORT_NUMBER)

    def close(self):
        self.socket.close()

    def erase(self, address, length):
        cmd = EraseCommand(address, length)
        ack_received = False
        retries = 0
        while retries < 10:
            if not ack_received:
                self.socket.send(cmd.packet)
            try:
                packet = self.socket.receive(timeout=5 if ack_received else 1.5)
                response = cmd.parse_response(packet)
                ack_received = True
                if response.complete:
                    return
            except pebble.pulse2.exceptions.ReceiveQueueEmpty:
                ack_received = False
                retries += 1
                continue
        raise exceptions.CommandTimedOut

    def write(self, address, data, max_retries=5, max_in_flight=5,
              progress_cb=None):
        mtu = self.socket.mtu - WriteCommand.header_len
        assert(mtu > 0)
        unsent = collections.deque()
        for offset in xrange(0, len(data), mtu):
            segment = data[offset:offset+mtu]
            assert(len(segment))
            seg_address = address + offset
            unsent.appendleft(
                    (seg_address, WriteCommand(seg_address, segment), 0))

        in_flight = collections.OrderedDict()
        retries = 0
        while unsent or in_flight:
            try:
                while True:
                    # Process ACKs (if any)
                    ack = WriteResponse.parse(
                            self.socket.receive(block=False))
                    try:
                        cmd, _, _ = in_flight[ack.address]
                        del in_flight[ack.address]
                    except KeyError:
                        for seg_address, cmd, retry_count in unsent:
                            if seg_address == ack.address:
                                if retry_count == 0:
                                    # ACK for a segment we never sent?!
                                    raise exceptions.WriteError(
                                        'Received ACK for an unsent segment: '
                                        '%#.08x' % ack.address)

                                # Got an ACK for a sent (but timed out) segment
                                unsent.remove((seg_address, cmd, retry_count))
                                break
                        else:
                            raise exceptions.WriteError(
                                'Received ACK for an unknown segment: '
                                '%#.08x' % ack.address)


                    if len(cmd.data) != ack.length:
                        raise exceptions.WriteError(
                                'ACK length %d != data length %d' % (
                                    ack.length, len(cmd.data)))
                    assert(ack.complete)
                    if progress_cb:
                        progress_cb(True)
            except pebble.pulse2.exceptions.ReceiveQueueEmpty:
                pass

            # Retry any in_flight writes where the ACK has timed out
            to_retry = []
            timeout_time = time.time() - 0.5
            for (seg_address,
                    (cmd, send_time, retry_count)) in in_flight.iteritems():
                if send_time > timeout_time:
                    # in_flight is an OrderedDict so iteration is in
                    # chronological order.
                    break
                if retry_count >= max_retries:
                    raise exceptions.WriteError(
                        'Segment %#.08x exceeded the max retry count (%d)' % (
                            seg_address, max_retries))
                # Enqueue the packet again to resend later.
                del in_flight[seg_address]
                unsent.appendleft((seg_address, cmd, retry_count+1))
                retries += 1
                if progress_cb:
                    progress_cb(False)

            # Send out fresh segments
            try:
                while len(in_flight) < max_in_flight:
                    seg_address, cmd, retry_count = unsent.pop()
                    self.socket.send(cmd.packet)
                    in_flight[cmd.address] = (cmd, time.time(), retry_count)
            except IndexError:
                pass

            # Give other threads a chance to run
            time.sleep(0)
        return retries

    def _command_and_response(self, cmd, timeout=0.5):
        for attempt in xrange(5):
            self.socket.send(cmd.packet)
            try:
                packet = self.socket.receive(timeout=timeout)
                return cmd.parse_response(packet)
            except pebble.pulse2.exceptions.ReceiveQueueEmpty:
                pass
        raise exceptions.CommandTimedOut

    def crc(self, address, length):
        cmd = CrcCommand(address, length)
        return self._command_and_response(cmd, timeout=1).crc

    def query_region_geometry(self, region):
        cmd = QueryFlashRegionCommand(region)
        return self._command_and_response(cmd)

    def finalize_region(self, region):
        cmd = FinalizeFlashRegionCommand(region)
        return self._command_and_response(cmd)
