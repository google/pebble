#!/usr/bin/python
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


from analyze_mcu_flash_config import *

import argparse
import binutils
import sh


def contains(a, b):
    """ True if b is inside a """
    return b[0] >= a[0] and b[1] <= a[1]


def claim(c, unclaimed_regions, symbol):
    """ Removes region (c_start, c_end) from the set of unclaimed_regions
        Return True if the region was successfully removed, False if it was
        already claimed.

    """
    if c[0] == c[1]:
        raise Exception("Invalid region: 0 size! %s" % c)

    for u in unclaimed_regions:
        if contains(u, c):
            unclaimed_regions.remove(u)

            # Defensive programming:
            if c[0] < u[0]:
                raise Exception("WTF! %s %s" % (u, c))
            if c[1] > u[1]:
                raise Exception("WTF! %s %s" % (u, c))

            if u[0] != c[0]:
                # Lower edge of the claimed region does not overlap with
                # the unclaimed region. Add a piece of unclaimed padding:
                unclaimed_regions.add((u[0], c[0]))
            if u[1] != c[1]:
                # Upper edge of the claimed region does not overlap with
                # the unclaimed region. Add a piece of unclaimed padding:
                unclaimed_regions.add((c[1], u[1]))
            return True

    print "Warning: doubly claimed %s, 0x%08x - 0x%08x?" % (symbol, c[0], c[1])
    return False


if (__name__ == '__main__'):
    parser = argparse.ArgumentParser()
    parser.add_argument('--verbose', action='store_true')
    parser.add_argument('--dump', action='store_true',
                        help='objdump unclaimed regions')
    parser.add_argument('--fast', action='store_true')
    parser.add_argument(
        '--config', default='tintin', choices=CONFIG_CLASSES.keys())
    parser.add_argument('elf_file', nargs='?')
    args = parser.parse_args()

    config_class = CONFIG_CLASSES[args.config]
    config = config_class()

    elf_file = args.elf_file
    if not elf_file:
        elf_file = config.default_elf_abs_path()

    # The set of (addr_start, addr_end) tuples that we use to keep track of
    # unclaimed space in the flash:
    unclaimed_regions = set([config.memory_region_to_analyze()])

    # Using arm-none-eabi-nm, 'claim' all .text symbols by removing the regions
    # from the unclaimed_regions set
    symbols = binutils.nm_generator(elf_file, args.fast)
    bytes_claimed = 0
    for addr, section, symbol, src_path, line, size in symbols:
        if section != 't':
            continue
        c = (addr, addr + size)
        if not contains(config.memory_region_to_analyze(), c):
            raise Exception("Not in memory region: %s 0x%08x - 0x%08x" %
                            (symbol, c[0], c[1]))
        claim(c, unclaimed_regions, symbol)
        bytes_claimed += size

    # Using the resulting map of unused space,
    # calculate the total unclaimed space:
    bytes_unclaimed = 0
    for u in unclaimed_regions:
        bytes_unclaimed += u[1] - u[0]

    # Print out the results
    text_size = binutils.size(elf_file)[0]
    region = config.memory_region_to_analyze()
    print "------------------------------------------------------------"
    print ".text:                            %u" % text_size
    print "unclaimed memory:                 %u" % bytes_unclaimed
    print "claimed memory:                   %u" % bytes_claimed
    print "unknown .text regions             %u" % (text_size - bytes_claimed)
    print ""
    print "These should add up:"
    print "bytes_unclaimed + bytes_claimed = %u" % (bytes_unclaimed +
                                                    bytes_claimed)
    print "REGION_END - REGION_START =       %u" % (region[1] - region[0])
    print ""

    num = 30
    print "------------------------------------------------------------"
    print "Top %u unclaimed memory regions:" % num

    def comparator(a, b):
        return cmp(a[1] - a[0], b[1] - b[0])
    unclaimed_sorted_by_size = sorted(unclaimed_regions,
                                      cmp=comparator, reverse=True)
    for x in xrange(0, num):
        region = unclaimed_sorted_by_size[x]
        size = region[1] - region[0]
        if args.dump:
            print "-----------------------------------------------------------"
            print "%u bytes @ 0x%08x" % (size, region[0])
            print ""
            print sh.arm_none_eabi_objdump('-S',
                                           '--start-address=0x%x' % region[0],
                                           '--stop-address=0x%x' % region[1],
                                           elf_file)
        else:
            print "%u bytes @ 0x%08x" % (size, region[0])

    print "------------------------------------------------------------"
    print "Unclaimed regions are regions that did map to symbols in the .elf."
