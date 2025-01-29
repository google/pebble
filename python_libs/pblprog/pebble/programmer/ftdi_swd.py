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

from array import array

from pyftdi.pyftdi.ftdi import Ftdi
import usb.util

class FTDISerialWireDebug(object):
    def __init__(self, vid, pid, interface, direction, output_mask, reset_mask, frequency):
        self._direction = direction
        self._output_mask = output_mask
        self._reset_mask = reset_mask
        self._ftdi = Ftdi()
        try:
            self._ftdi.open_mpsse(vid, pid, interface, direction=direction, frequency=frequency,
                                  latency=1)
        except:
            self._ftdi = None
            raise

        # get the FTDI FIFO size and increase the chunksize to match
        self._ftdi_fifo_size = min(self._ftdi.fifo_sizes)
        self._ftdi.write_data_set_chunksize(self._ftdi_fifo_size)

        self._cmd_buffer = array('B')
        self._output_enabled = False
        self._pending_acks = 0
        self._sequence_cmd_buffer = None

    def close(self):
        if not self._ftdi:
            return
        self.send_cmds()
        self._ftdi.close()
        # PyFTDI doesn't do a good job of cleaning up - make sure we release the usb device
        usb.util.dispose_resources(self._ftdi.usb_dev)
        self._ftdi = None

    def _fatal(self, message):
        raise Exception('FATAL ERROR: {}'.format(message))

    def _queue_cmd(self, write_data):
        if len(write_data) > self._ftdi_fifo_size:
            raise Exception('Data too big!')
        if self._sequence_cmd_buffer is not None:
            self._sequence_cmd_buffer.extend(write_data)
        else:
            if len(self._cmd_buffer) + len(write_data) > self._ftdi_fifo_size:
                self.send_cmds()
            self._cmd_buffer.extend(write_data)

    def _set_output_enabled(self, enabled):
        if enabled == self._output_enabled:
            return
        self._output_enabled = enabled
        direction = self._direction & ~(0x00 if enabled else self._output_mask)
        self._queue_cmd([Ftdi.SET_BITS_LOW, 0, direction])

    def reset(self):
        # toggle the reset line
        self.reset_lo()
        self.reset_hi()

    def reset_lo(self):
        direction = self._direction & ~(0x00 if self._output_enabled else self._output_mask)
        self._queue_cmd([Ftdi.SET_BITS_LOW, 0, direction | self._reset_mask])
        self.send_cmds()

    def reset_hi(self):
        direction = self._direction & ~(0x00 if self._output_enabled else self._output_mask)
        self._queue_cmd([Ftdi.SET_BITS_LOW, 0, direction & ~self._reset_mask])
        self.send_cmds()

    def send_cmds(self):
        if self._sequence_cmd_buffer is not None:
            self._ftdi.write_data(self._sequence_cmd_buffer)
        elif len(self._cmd_buffer) > 0:
            self._ftdi.write_data(self._cmd_buffer)
            self._cmd_buffer = array('B')

    def write_bits_cmd(self, data, num_bits):
        if num_bits < 0 or num_bits > 8:
            self._fatal('Invalid num_bits')
        elif (data & ((1 << num_bits) - 1)) != data:
            self._fatal('Invalid data!')
        self._set_output_enabled(True)
        self._queue_cmd([Ftdi.WRITE_BITS_NVE_LSB, num_bits - 1, data])

    def write_bytes_cmd(self, data):
        length = len(data) - 1
        if length < 0 or length > 0xffff:
            self._fatal('Invalid length')
        self._set_output_enabled(True)
        self._queue_cmd([Ftdi.WRITE_BYTES_NVE_LSB, length & 0xff, length >> 8] + data)

    def read_bits_cmd(self, num_bits):
        if num_bits < 0 or num_bits > 8:
            self._fatal('Invalid num_bits')
        self._set_output_enabled(False)
        self._queue_cmd([Ftdi.READ_BITS_PVE_LSB, num_bits - 1])

    def read_bytes_cmd(self, length):
        length -= 1
        if length < 0 or length > 0xffff:
            self._fatal('Invalid length')
        self._set_output_enabled(False)
        self._queue_cmd([Ftdi.READ_BYTES_PVE_LSB, length & 0xff, length >> 8])

    def get_read_bytes(self, length):
        return self._ftdi.read_data_bytes(length)

    def get_read_fifo_size(self):
        return self._ftdi_fifo_size

    def start_sequence(self):
        if self._sequence_cmd_buffer is not None:
            self._fatal('Attempted to start a sequence while one is in progress')
        self.send_cmds()
        self._sequence_cmd_buffer = array('B')

    def end_sequence(self):
        if self._sequence_cmd_buffer is None:
            self._fatal('No sequence started')
        self._sequence_cmd_buffer = None

