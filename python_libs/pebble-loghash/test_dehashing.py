# Copyright 2025 Google LLC
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

#/usr/bin/env python
"""
Tests for pebble.loghashing.dehashing
"""
LOOKUP_DICT = {"13108": "activity.c:activity tracking started",
               "45803": "ispp.c:Start Authentication Process (%d) %s"}

from pebble.loghashing.dehashing import (dehash_line, parse_line, parse_support_line, parse_message,
                                         dehash_str, parse_args)

def test_dehash_file():
    """
    Test for dehash_file()
    """
    pass

def test_dehash_line():
    """
    Test for dehash_line()
    """
    # Console Line - No arguments
    assert ("D A 21:35:14.375 activity.c:804> activity tracking started" ==
            dehash_line("D A 21:35:14.375 :804> LH:3334", LOOKUP_DICT))

    # Console Line - Arguments
    assert ("D A 21:35:14.375 ispp.c:872> Start Authentication Process (2) Success" ==
            dehash_line("D A 21:35:14.375 :872> LH:b2eb 2 `Success`", LOOKUP_DICT))

    # Support Line - No arguments
    assert ("2015-09-05 02:16:16:000GMT activity.c:804> activity tracking started" ==
            dehash_line("2015-09-05 02:16:16:000GMT :804 LH:3334", LOOKUP_DICT))

    # Support Line - Arguments
    assert ("2015-09-05 02:16:19:000GMT ispp.c:872> Start Authentication Process (2) Success" ==
            dehash_line("2015-09-05 02:16:19:000GMT :872 LH:b2eb 2 `Success`", LOOKUP_DICT))

def test_parse_line():
    """
    Test for parse_line()
    """
    # No arguments
    assert ("D A 21:35:14.375 activity.c:804> activity tracking started" ==
            parse_line("D A 21:35:14.375 :804> LH:3334", LOOKUP_DICT))

    # Arguments
    assert ("D A 21:35:14.375 ispp.c:872> Start Authentication Process (2) Success" ==
            parse_line("D A 21:35:14.375 :872> LH:b2eb 2 `Success`", LOOKUP_DICT))

def test_parse_support_line():
    """
    Test for parse_support_line()
    """
    # No arguments
    assert ("2015-09-05 02:16:16:000GMT activity.c:804> activity tracking started" ==
            parse_support_line("2015-09-05 02:16:16:000GMT :804 LH:3334", LOOKUP_DICT))

    # Arguments
    assert ("2015-09-05 02:16:19:000GMT ispp.c:872> Start Authentication Process (2) Success" ==
            parse_support_line("2015-09-05 02:16:19:000GMT :872 LH:b2eb 2 `Success`", LOOKUP_DICT))

def test_parse_message():
    """
    Test for parse_message()
    """
    # Console Line - No arguments
    assert ({'msg': 'activity tracking started', 'line': '804', 'file': 'activity.c'} ==
            parse_message(":804> LH:3334", LOOKUP_DICT))

    # Console Line - Arguments
    assert ({'msg': 'Start Authentication Process (2) Success', 'line': '872', 'file': 'ispp.c'} ==
            parse_message(":872> LH:b2eb 2 `Success`", LOOKUP_DICT))

    # Support Line - No arguments
    assert ({'msg': 'activity tracking started', 'line': '804', 'file': 'activity.c'} ==
            parse_message(":804 LH:3334", LOOKUP_DICT))

    # Support Line - Arguments
    assert ({'msg': 'Start Authentication Process (2) Success', 'line': '872', 'file': 'ispp.c'} ==
            parse_message(":872 LH:b2eb 2 `Success`", LOOKUP_DICT))

def test_dehash_str():
    """
    Test for dehash_str()
    """
    # No arguments
    assert ("activity.c:activity tracking started" ==
            dehash_str("3334", LOOKUP_DICT))

    # Arguments
    assert ("ispp.c:Start Authentication Process (%d) %s" ==
            dehash_str("b2eb", LOOKUP_DICT))

def test_parse_args():
    """
    Test for parse_args()
    """
    # No `` delimited strings
    assert ["foo", "bar", "baz"] == parse_args("foo bar baz")

    # `` delimited strings
    assert ["foo", "bar baz"] == parse_args("foo `bar baz`")
