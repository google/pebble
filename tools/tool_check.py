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

import re
import sh
import subprocess
import sys
from pkg_resources import parse_version

from waflib import Logs

REQUIREMENTS_OSX = 'requirements-osx.txt'
REQUIREMENTS_BREW = 'requirements-brew.txt'

VERSION_REGEX = r"^(?P<package>.*)(?P<comparator>==|<=|>=|<|>)(?P<version>.*)"
VERSION_PATTERN = re.compile(VERSION_REGEX)


def tool_check():
    if sys.platform.startswith('linux'):
        Logs.pprint('RED', 'Linux requirements-linux.txt checking not implemented. Sorry.')
        return

    Logs.pprint('CYAN', 'Checking %s' % REQUIREMENTS_OSX)

    with open(REQUIREMENTS_OSX) as file:
        osx_req_list = text_to_req_list(file.read())

    pip_installed_text = sh.pip('freeze')
    pip_installed_dict = installed_list_to_dict(text_to_req_list(pip_installed_text))

    for req in osx_req_list:
        check_requirement(req, pip_installed_dict)

    Logs.pprint('CYAN', 'Checking %s' % REQUIREMENTS_BREW)

    with open(REQUIREMENTS_BREW) as file:
        brew_req_text = file.read()
        brew_req_list = text_to_req_list(brew_req_text)

    brew_installed_text = subprocess.check_output(['brew', 'list'])
    brew_installed_dict = installed_list_to_dict(text_to_req_list(brew_installed_text))

    for req in brew_req_list:
        check_requirement(req, brew_installed_dict)


def installed_list_to_dict(list):
    d = {}

    for l in list:
        d[l[0]] = l[2]

    return d


def text_to_req_list(req_list_text):
    req_list = []

    for raw_line in req_list_text.splitlines():
        # Recursively handle -r includes
        if raw_line.startswith('-r '):
            with open(raw_line[3:]) as file:
                req_list.extend(text_to_req_list(file.read()))
            continue

        # Remove whitespace, comments, & --extra-index-url
        line = raw_line.replace(" ", "")
        if len(line) < 2 or line.startswith('#') or line.startswith('--'):
            continue

        match = VERSION_PATTERN.match(line)
        if not match:
            # Assume no version info
            req_list.append((line, None, None))
            continue
        if match.group('package').endswith(','):
            # Multiple requirements
            match2 = VERSION_PATTERN.match(match.group('package').strip(','))
            if not match2:
                Logs.pprint('RED', 'Don\'t understand line \'%s\'' % raw_line)
                continue
            req_list.append((match2.group('package'), match2.group('comparator'),
                             match2.group('version')))
            req_list.append((match2.group('package'), match.group('comparator'),
                             match.group('version')))
        else:
            req_list.append((match.group('package'), match.group('comparator'),
                             match.group('version')))

    return req_list


def check_requirement(req, installed):
    if req[0] not in installed:
        Logs.pprint('RED', 'Package \'%s\' not installed' % req[0])
        return

    if not req[1]:
        # No version/comparison
        return

    version = parse_version(installed[req[0]])
    success = True

    if req[1] == '==':
        success = version == parse_version(req[2])
    elif req[1] == '<=':
        success = version <= parse_version(req[2])
    elif req[1] == '>=':
        success = version >= parse_version(req[2])
    elif req[1] == '<':
        success = version < parse_version(req[2])
    elif req[1] == '>':
        success = version > parse_version(req[2])
    else:
        Logs.pprint('RED', 'Don\'t understand comparison \'%s\'' % req[1])

    if not success:
        Logs.pprint('RED', 'Package \'%s\' installed = %s, needed %s %s ' %
                    (req[0], version, req[1], req[2]))


# vim:filetype=python
