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
Module for de-hashing log strings
"""

from pebble.loghashing.constants import (LOG_LINE_CONSOLE_PATTERN, LOG_LINE_SUPPORT_PATTERN,
                                         LOG_MSG_PATTERN, DEHASHED_MSG_PATTERN, HASHED_INFO_PATTERN,
                                         FORMAT_TAG_PATTERN)

from pebble.loghashing.newlogging import dehash_line as newlogging_dehash_line
from pebble.loghashing.newlogging import LOG_DICT_KEY_VERSION

def dehash_file(file_name, lookup_dict):
    """
    Dehash a file

    :param file_name: Path of the file to dehash
    :type file_name: str
    :param lookup_dict: Hash lookup dictionary
    :type lookup_dict: dict

    :returns: A list containing the dehashed lines
    """
    # Grab the lines from the file
    with open(file_name, 'r') as fp:
        lines = fp.readlines()

    # Dehash the lines
    lines = [dehash_line(x, lookup_dict) + "\n" for x in lines]

    return lines

def dehash_line(line, lookup_dict):
    """
    Dehash a line

    :param line: The line to dehash
    :type line: str
    :param lookup_dict: Hash lookup dictionary
    :type lookup_dict: dict

    If the lookup dictionary contains the 'new_logging_version' key, it's a newlogging style
    print. Pass it off to the appropriate handler.

    :returns: A string containing the dehashed line, or the submitted line.
    """
    if LOG_DICT_KEY_VERSION in lookup_dict:
        return newlogging_dehash_line(line, lookup_dict)

    return parse_line(line, lookup_dict) or parse_support_line(line, lookup_dict) or line

def parse_line(line, lookup_dict):
    """
    Parse a log line

    :param msg: The line to parse
    :type msg: str
    :param lookup_dict: Hash lookup dictionary
    :type lookup_dict: dict

    :returns: A string containing the parsed line, or a null string.
    """
    match = LOG_LINE_CONSOLE_PATTERN.search(line)

    output = ""

    if match:
        parsed = parse_message(match.group('msg'), lookup_dict)

        output = "{} {} {} {}:{}> {}".format(match.group('re_level'), match.group('task'),
                                             match.group('time'), parsed['file'],
                                             parsed['line'], parsed['msg'])

    return output

def parse_support_line(line, lookup_dict):
    """
    Parse a log line

    :param msg: The line to parse
    :type msg: str
    :param lookup_dict: Hash lookup dictionary
    :type lookup_dict: dict

    :returns: A string containing the parsed line, or a null string.
    """
    match = LOG_LINE_SUPPORT_PATTERN.search(line)

    output = ""

    if match:
        parsed = parse_message(match.group('msg'), lookup_dict)

        output = "{} {} {}:{}> {}".format(match.group('date'), match.group('time'),
                                          parsed['file'], parsed['line'], parsed['msg'])

    return output

def parse_message(msg, lookup_dict):
    """
    Parse the log message part of a line

    :param msg: The message to parse
    :type msg: str
    :param lookup_dict: Hash lookup dictionary
    :type lookup_dict: dict

    :returns: A dictionary containing the parsed message, file name, and line number
    """
    output = {'msg':msg, 'file':"", 'line':""}

    match = LOG_MSG_PATTERN.search(msg)

    if match:
        output['file'] = match.group('f')
        output['line'] = match.group('l')
        hashed = match.group('h')

        dehashed_str = dehash_str(hashed, lookup_dict)

        output['msg'] = "LH:{}".format(dehashed_str)

        match2 = DEHASHED_MSG_PATTERN.search(dehashed_str)

        if match2:
            output['file'] = match2.group(1) or output['file']
            output['line'] = match2.group(2) or output['line']
            output['msg'] = match2.group(3) or dehashed_str

    return output

def dehash_str(hashed_info, lookup_dict):
    """
    Search the lookup dictionary for a match, and return the dehashed string

    :param hashed_info: Hash and arguments
    :type hashed_info: str

    :returns: A string with after doing a hash lookup, and substituting arguments
    """
    match = HASHED_INFO_PATTERN.search(hashed_info)

    # If there's no mach, return the hashed info as the log message
    output = hashed_info

    if match:
        # Look for the hex value in the dictionary keys
        # If we can't find a match, set formatted string to hashed_info
        formatted_string = lookup_dict.get(str(match.group('hash_key')), hashed_info)

        # If we couldn't find a match, try converting to base 10 to find a match
        # If we can't find a match, set formatted string to hashed_info
        if formatted_string == hashed_info:
            formatted_string = lookup_dict.get(str(int(match.group('hash_key'), 16)), hashed_info)

        # For each argument, substitute a C-style format specifier in the string
        for arg in parse_args(match.group('arg_list')):
            formatted_string = FORMAT_TAG_PATTERN.sub(arg, formatted_string, 1)

        # Return the filename, and log message
        output = formatted_string

    return output

def parse_args(raw_args):
    """
    Split the argument list, taking care of `delimited strings`
    Idea taken from http://bit.ly/1KHzc0y

    :param raw_args: Raw argument list
    :type raw_args: str

    :returns: A list containing the arguments
    """
    args = []
    arg_run = []
    in_str = False

    if raw_args:
        for arg_ch in raw_args:
            # Start or stop of a ` delimited string
            if arg_ch == "`":
                in_str = not in_str
            # If we find a space, and we're not in a ` delimited string, this is a Boundary
            elif arg_ch == " " and not in_str:
                args.append("".join(arg_run).strip())
                arg_run = []
            else:
                arg_run.append(arg_ch)
        if arg_run:
            args.append("".join(arg_run).strip())
    return args
