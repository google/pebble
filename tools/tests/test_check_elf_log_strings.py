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

import os
import sys
import unittest

# Allow us to run even if not at the `tools` directory.
root_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), os.pardir))
sys.path.insert(0, root_dir)

from log_hashing.check_elf_log_strings import check_dict_log_strings


class TestCheckLogStrings(unittest.TestCase):

    def test_some_acceptable_strings(self):
        log_dict = {
                       1: {'file': 'test.c', 'line': '1', 'msg': 'test %s'},
                       2: {'file': 'test.c', 'line': '2', 'msg': 'test %-2hx'},
                       3: {'file': 'test.c', 'line': '3', 'msg': 'test %08X'},
                       4: {'file': 'test.c', 'line': '4', 'msg': 'test %14d'},
                       5: {'file': 'test.c', 'line': '5', 'msg': 'test %p %d %02X %x'}
                   }

        output = check_dict_log_strings(log_dict)

        self.assertEquals(output, '')

    def test_rule_no_backtick(self):
        log_dict = {1: {'file': 'test.c', 'line': '1', 'msg': 'test ` '}}
        file_line = ':'.join((log_dict[1]['file'], log_dict[1]['line']))

        output = check_dict_log_strings(log_dict)
        self.assertTrue(file_line in output and "PBL_LOG contains '`'" in output)

    def test_rule_no_percent(self):
        log_dict = {1: {'file': 'test.c', 'line': '1', 'msg': 'test 10%%'}}
        file_line = ':'.join((log_dict[1]['file'], log_dict[1]['line']))

        output = check_dict_log_strings(log_dict)
        self.assertTrue(file_line in output and "PBL_LOG contains '%%'" in output)

    def test_rule_no_64_bit(self):
        log_dict = {1: {'file': 'test.c', 'line': '1', 'msg': 'test %llu'}}
        file_line = ':'.join((log_dict[1]['file'], log_dict[1]['line']))

        output = check_dict_log_strings(log_dict)
        self.assertTrue(file_line in output and "PBL_LOG contains 64 bit value" in output)

    def test_rule_no_floats(self):
        log_dict = {1: {'file': 'test.c', 'line': '1', 'msg': 'test %6.2f'}}
        file_line = ':'.join((log_dict[1]['file'], log_dict[1]['line']))

        output = check_dict_log_strings(log_dict)
        self.assertTrue(file_line in output and
                        "PBL_LOG contains floating point specifier" in output)

    def test_rule_no_formatted_strings(self):
        log_dict = {1: {'file': 'test.c', 'line': '1', 'msg': 'test %.*s'}}
        file_line = ':'.join((log_dict[1]['file'], log_dict[1]['line']))

        output = check_dict_log_strings(log_dict)
        self.assertTrue(file_line in output and
                        "PBL_LOG contains a formatted string conversion" in output)

    def test_rule_dynamic_width(self):
        log_dict = {1: {'file': 'test.c', 'line': '1', 'msg': 'test %*d'}}
        file_line = ':'.join((log_dict[1]['file'], log_dict[1]['line']))

        output = check_dict_log_strings(log_dict)
        self.assertTrue(file_line in output and
                        "PBL_LOG contains a dynamic width" in output)

    def test_rule_no_flagged_strings(self):
        log_dict = {1: {'file': 'test.c', 'line': '1', 'msg': 'test %-10s'}}
        file_line = ':'.join((log_dict[1]['file'], log_dict[1]['line']))

        output = check_dict_log_strings(log_dict)
        self.assertTrue(file_line in output and
                        "PBL_LOG contains a formatted string conversion" in output)

    def test_rule_7_conversions(self):
        log_dict = {1: {'file': 'test.c', 'line': '1', 'msg': 'test %s %d %0d %x %08X %s %p %ul'}}
        file_line = ':'.join((log_dict[1]['file'], log_dict[1]['line']))

        output = check_dict_log_strings(log_dict)
        self.assertTrue(file_line in output and
                        "PBL_LOG contains more than 7 format conversions" in output)

    def test_rule_2_string_conversions(self):
        log_dict = {1: {'file': 'test.c', 'line': '1', 'msg': 'test %s %08X %s %s %ul'}}
        file_line = ':'.join((log_dict[1]['file'], log_dict[1]['line']))

        output = check_dict_log_strings(log_dict)
        self.assertTrue(file_line in output and
                        "PBL_LOG contains more than 2 string conversions" in output)

    def test_rule_unknown_specifier(self):
        log_dict = {1: {'file': 'test.c', 'line': '1', 'msg': 'test %Z'}}
        file_line = ':'.join((log_dict[1]['file'], log_dict[1]['line']))

        output = check_dict_log_strings(log_dict)
        self.assertTrue(file_line in output and
                        "PBL_LOG contains unknown format specifier" in output)

    def test_rule_no_specifier(self):
        log_dict = {1: {'file': 'test.c', 'line': '1', 'msg': '%'}}
        file_line = ':'.join((log_dict[1]['file'], log_dict[1]['line']))

        output = check_dict_log_strings(log_dict)
        self.assertTrue(file_line in output and
                        "PBL_LOG contains unknown format specifier" in output)

    def test_valid_level_number(self):
        log_dict = {1: {'file': 'test.c', 'line': '1', 'msg': '%', 'level': '69'}}
        file_line = ':'.join((log_dict[1]['file'], log_dict[1]['line']))

        output = check_dict_log_strings(log_dict)
        self.assertTrue(file_line in output and
                        "PBL_LOG contains a non-constant LOG_LEVEL_ value '69'" in output)

    def test_valid_level_line(self):
        log_dict = {1: {'file': 'test.c', 'line': '1', 'msg': '%', 'level': 'variable_name'}}
        file_line = ':'.join((log_dict[1]['file'], log_dict[1]['line']))

        output = check_dict_log_strings(log_dict)
        self.assertTrue(file_line in output and
                        "PBL_LOG contains a non-constant LOG_LEVEL_ value 'variable_name'"
                        in output)


if __name__ == '__main__':
    unittest.main()
