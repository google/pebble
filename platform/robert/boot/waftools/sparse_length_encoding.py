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


"""
Sparse Length Encoding

A variant of run-length encoding which is tuned specifically to encode binary
data with long runs of zeroes interspersed with random (poorly-compressible)
data.

The format is fairly simple. The encoded data is a stream of octets (bytes)
beginning with a one-octet header. This header octet is the 'escape byte' that
indicates to the decoder that it and the following octets should be treated
specially. The encoder selects this escape byte to be an octet which occurs
least frequently (or not at all) in the decoded data.

The following octets of the encoded data are emitted literally until an escape
byte is encountered. The escape byte marks the start of  an 'escape sequence'
comprised of the escape byte itself and one or two following bytes.

 - The escape byte followed by 0x00 indicates the end of input.
 - The escape byte followed by 0x01 means 'emit a literal escape byte'
 - The escape byte followed by a byte "b" between 0x02 and 0x7f inclusive means
   'emit b zeroes'. This two-byte sequence can encode a run of length 2-127.
 - The escape byte followed by a byte "b" equal to or greater than 0x80
   (i.e. with the MSB set) means 'take the next byte "c" and emit
   ((b & 0x7f) << 8 | c)+0x80 zeroes'. This three-byte sequence can encode a run
   of length 128-32895.

The minimum overhead for this encoding scheme is three bytes: header and
end-of-input escape sequence.
"""

from collections import Counter
from itertools import groupby

_MAX_COUNT = 0x807F  # max is ((0x7F << 8) | (0xFF) + 0x80


def encode(source):
    # Analyze the source data to select the escape byte. To keep things simple, we don't allow 0 to
    # be the escape character.
    source = bytes(source)
    frequency = Counter({chr(n): 0 for n in range(1, 256)})
    frequency.update(source)
    # most_common() doesn't define what happens if there's a tie in frequency. Let's always pick
    # the lowest value of that frequency to make the encoding predictable.
    occurrences = frequency.most_common()
    escape = min(x[0] for x in occurrences if x[1] == occurrences[-1][1])
    yield escape
    for b, g in groupby(source):
        if b == b'\0':
            # this is a run of zeros
            count = len(list(g))
            while count >= 0x80:
                # encode the number of zeros using two bytes
                unit = min(count, _MAX_COUNT)
                count -= unit
                unit -= 0x80
                yield escape
                yield chr(((unit >> 8) & 0x7F) | 0x80)
                yield chr(unit & 0xFF)
            if count == 1:
                # can't encode a length of 1 zero, so just emit it directly
                yield b
            elif 1 < count < 0x80:
                # encode the number of zeros using one byte
                yield escape
                yield chr(count)
            elif count < 0:
                raise Exception('Encoding malfunctioned')
        else:
            # simply insert the characters (and escape the escape character)
            for _ in g:
                yield b
                if b == escape:
                    yield b'\1'
    yield escape
    yield b'\0'


def decode(stream):
    stream = iter(stream)
    escape = next(stream)
    while True:
        char = next(stream)
        if char == escape:
            code = next(stream)
            if code == b'\0':
                return
            elif code == b'\1':
                yield escape
            else:
                if ord(code) & 0x80 == 0:
                    count = ord(code)
                else:
                    count = (((ord(code) & 0x7f) << 8) | ord(next(stream))) + 0x80
                    assert(count <= _MAX_COUNT)
                for _ in xrange(count):
                    yield b'\0'
        else:
            yield char


if __name__ == '__main__':
    import sys
    if len(sys.argv) == 1:
        # run unit tests
        import unittest

        class TestSparseLengthEncoding(unittest.TestCase):
            def test_empty(self):
                raw_data = ''
                encoded_data = ''.join(encode(raw_data))
                decoded_data = ''.join(decode(encoded_data))
                self.assertEquals(encoded_data, '\x01\x01\x00')

            def test_no_zeros(self):
                raw_data = '\x02\xff\xef\x99'
                encoded_data = ''.join(encode(raw_data))
                decoded_data = ''.join(decode(encoded_data))
                self.assertEquals(encoded_data, '\x01\x02\xff\xef\x99\x01\x00')

            def test_one_zero(self):
                raw_data = '\x00'
                encoded_data = ''.join(encode(raw_data))
                decoded_data = ''.join(decode(encoded_data))
                self.assertEquals(encoded_data, '\x01\x00\x01\x00')

            def test_small_number_of_zeros(self):
                # under 0x80 zeros
                raw_data = '\0' * 0x0040
                encoded_data = ''.join(encode(raw_data))
                decoded_data = ''.join(decode(encoded_data))
                self.assertEquals(encoded_data, '\x01\x01\x40\x01\x00')
                self.assertEquals(decoded_data, raw_data)

            def test_medium_number_of_zeros(self):
                # between 0x80 and 0x807f zeros
                raw_data = '\0' * 0x1800
                encoded_data = ''.join(encode(raw_data))
                decoded_data = ''.join(decode(encoded_data))
                self.assertEquals(encoded_data, '\x01\x01\x97\x80\x01\x00')
                self.assertEquals(decoded_data, raw_data)

            def test_remainder_one(self):
                # leaves a remainder of 1 zero
                raw_data = '\0' * (0x807f + 1)
                encoded_data = ''.join(encode(raw_data))
                decoded_data = ''.join(decode(encoded_data))
                self.assertEquals(encoded_data, '\x01\x01\xff\xff\x00\x01\x00')
                self.assertEquals(decoded_data, raw_data)

            def test_remainder_under_128(self):
                # leaves a remainder of 100 zeros
                raw_data = '\0' * (0x807f + 100)
                encoded_data = ''.join(encode(raw_data))
                decoded_data = ''.join(decode(encoded_data))
                self.assertEquals(encoded_data, '\x01\x01\xff\xff\x01\x64\x01\x00')
                self.assertEquals(decoded_data, raw_data)

        unittest.main()
    elif len(sys.argv) == 2:
        # encode the specified file
        data = open(sys.argv[1], 'rb').read()
        encoded = ''.join(encode(data))
        if ''.join(decode(encoded)) != data:
            raise Exception('Invalid encoding')
        sys.stdout.write(''.join(encode(f)))
    else:
        raise Exception('Invalid arguments')
