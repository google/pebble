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


from __future__ import print_function
from struct import pack, unpack
from collections import OrderedDict

import os
import sys
import zipfile
import argparse
import json
import time
import stm32_crc
import socket
import pprint

MANIFEST_VERSION = 2
BUNDLE_PREFIX = 'bundle'

class MissingFileException(Exception):
    def __init__(self, filename):
        self.filename = filename

def flen(path):
    statinfo = os.stat(path)
    return statinfo.st_size

def stm32crc(path):
    with open(path, 'r+b') as f:
        binfile = f.read()
        return stm32_crc.crc32(binfile) & 0xFFFFFFFF

def check_paths(*args):
    for path in args:
        if not os.path.exists(path):
            raise MissingFileException(path)

class PebbleBundle(object):
    def __init__(self, subfolder=None):
        self.generated_at = int(time.time())
        self.bundle_manifest = {
            'manifestVersion' : MANIFEST_VERSION,
            'generatedAt' : self.generated_at,
            'generatedBy' : socket.gethostname(),
            'debug' : {},
        }
        self.bundle_files = []
        self.subfolder = subfolder
        self.has_firmware = False
        self.has_appinfo = False
        self.has_layouts = False
        self.has_watchapp = False
        self.has_worker = False
        self.has_resources = False
        self.has_jsapp = False
        self.has_loghash = False
        self.has_children = False
        self.has_license = False
        self.has_jstooling = False
        self.rocky_info = {}

    def add_firmware(self,
                     firmware_path,
                     firmware_type,
                     firmware_timestamp,
                     firmware_commit,
                     firmware_hwrev,
                     firmware_version_tag):
        if self.has_firmware:
            raise Exception("Added multiple firmwares to a single bundle")

        if self.has_watchapp or self.has_worker:
            raise Exception("Cannot add firmware and watchapp to a single bundle")

        if firmware_type != 'normal' and \
                firmware_type != 'recovery':
            raise Exception("Invalid firmware type!")

        check_paths(firmware_path)
        self.type = 'firmware'
        self.bundle_files.append(firmware_path)
        self.bundle_manifest['firmware'] = {
            'name' : os.path.basename(firmware_path),
            'type' : firmware_type,
            'timestamp' : firmware_timestamp,
            'commit' : firmware_commit,
            'hwrev' : firmware_hwrev,
            'size' : flen(firmware_path),
            'crc' : stm32crc(firmware_path),
            'versionTag' : firmware_version_tag,
        }

        self.has_firmware = True
        return True

    def add_resources(self, resources_path, resources_timestamp, sdk_version=None):
        if self.has_resources:
            raise Exception("Added multiple resource packs to a single bundle")

        check_paths(resources_path)
        self.bundle_files.append(resources_path)

        self.bundle_manifest['resources'] = {
            'name' : os.path.basename(resources_path),
            'timestamp' : resources_timestamp,
            'size' : flen(resources_path),
            'crc' : stm32crc(resources_path),
        }

        # If this is a SDK-built project that is 3.x or later, check for a layouts.json file
        if sdk_version is not None and sdk_version['major'] >= 5 and sdk_version['minor'] > 19:
            timeline_resource_path = os.path.join('build', self.subfolder, 'layouts.json')
       
            # If a project doesn't contain a resource json file, don't create an app_layouts object
            if os.path.exists(timeline_resource_path):
                self.bundle_files.append(timeline_resource_path)
                self.bundle_manifest['app_layouts'] = os.path.basename(timeline_resource_path)

        self.has_resources = True
        return True

    def add_loghash(self, loghash_path):
        if self.has_loghash:
            raise Exception("Added multiple loghash to a single bundle")

        check_paths(loghash_path)
        self.bundle_files.append(loghash_path)

        self.has_loghash = True
        return True

    def add_license(self, license_path):
        if self.has_license:
            raise Exception("Added multiple license to a single bundle")

        check_paths(license_path)
        self.bundle_files.append(license_path)

        self.has_license = True
        return True

    def add_jstooling(self, jstooling_path, bytecode_version):
        if self.has_jstooling:
            raise Exception("Added multiple js_toolings to a single bundle")

        if not (1 <= bytecode_version <= 31):
            raise Exception("Invalid bytecode version {}".format(bytecode_version))

        check_paths(jstooling_path)
        self.bundle_files.append(jstooling_path)

        self.bundle_manifest['js_tooling'] = {
            'bytecode_version': bytecode_version
        }

        self.has_jstooling = True
        return True

    def add_rockyjs(self, rocky_path, parent_bundle=None):
        """
        Add a rocky-app.js source file to the PBW bundle
        :param rocky_path: the path to the source rocky-app.js file in the project build folder
        :param parent_bundle: the parent PebbleBundle to write rocky-app.js to, or None if
        rocky-app.js should be written to the current platform subfolder
        :return: boolean indicating success or failure of addition
        """
        if self.rocky_info:
            raise Exception("PBW already has a rocky-app.js file")

        # Check to see that rocky-app.js source file exists in the build folder
        check_paths(rocky_path)

        if parent_bundle:
            # If rocky-app.js should be written to the parent_bundle (PBW root) of this
            # platform/bundle, construct a relative path for 'source_path' in manifest.json
            if rocky_path not in parent_bundle.bundle_files:
                parent_bundle.bundle_files.append(rocky_path)
            rocky_relative_path = '../' + os.path.basename(rocky_path)
        else:
            # If rocky-app.js should be written to this platform/subfolder, use the rocky-app.js
            # basename for 'source_path' in manifest.json
            self.bundle_files.append(rocky_path)
            rocky_relative_path = os.path.basename(rocky_path)
        self.rocky_info = {
            'source_path': rocky_relative_path
        }
        return True

    def add_appinfo(self, appinfo_path):
        if self.has_appinfo:
            raise Exception("Added multiple appinfo to a single bundle")

        check_paths(appinfo_path)
        self.bundle_files.append(appinfo_path)

        self.has_appinfo = True
        return True

    def add_layouts(self, layouts_path):
        if self.has_layouts:
            raise Exception("Added multiple layouts maps to a single bundle")
        check_paths(layouts_path)
        self.bundle_files.append(layouts_path)

        self.has_layouts = True
        return True

    def add_watchapp(self, watchapp_path, app_timestamp, sdk_version):
        if self.has_watchapp:
            raise Exception("Added multiple apps to a single bundle")

        if self.has_firmware:
            raise Exception("Cannot add watchapp and firmware to a single bundle")

        if sdk_version['major'] == 5 and sdk_version['minor'] < 20:
            self.bundle_manifest['manifestVersion'] = 1

        self.type = 'application'
        self.bundle_files.append(watchapp_path)
        self.bundle_manifest['application'] = {
            'timestamp': app_timestamp,
            'sdk_version': sdk_version,
            'name' : os.path.basename(watchapp_path),
            'size': flen(watchapp_path),
            'crc': stm32crc(watchapp_path),
        }
        self.has_watchapp = True
        return True

    def add_worker(self, worker_bin_path, worker_timestamp, sdk_version):
        if self.has_worker:
            raise Exception("Added multiple workers to a single bundle")

        if self.has_firmware:
            raise Exception("Cannot add worker and firmware to a single bundle")

        self.bundle_files.append(worker_bin_path)

        worker_name = os.path.basename(worker_bin_path)
        worker_size = flen(worker_bin_path)
        worker_crc = stm32crc(worker_bin_path)

        # NOTE: The type really should not be changed from 'application', but the 2.4 version of
        #  the iOS app will only install background worker apps when the type is set to
        #  'worker'. All newer versions of the iOS app allow the correct name of 'application'
        #  and version 2.5 accepts either.
        self.type = 'worker'
        self.bundle_manifest['worker'] = {
            'timestamp': worker_timestamp,
            'sdk_version': sdk_version,
            'name': worker_name,
            'size': worker_size,
            'crc': worker_crc,
        }
        self.has_worker = True
        return True

    def add_jsapp(self, js_files):
        if self.has_jsapp:
            raise Exception("Added multiple js apps to single bundle")

        check_paths(*js_files)

        for f in js_files:
            self.bundle_files.append(f)

        self.has_jsapp = True
        return True

    def write(self, out_path = None, verbose = False):
        if not (self.has_firmware or self.has_watchapp):
            raise Exception("Bundle must contain either a firmware or watchapp")


        if not out_path:
            out_path = 'pebble-{}-{:d}.pbz'.format(self.type, self.generated_at)

        if verbose:
            pprint.pprint(self.bundle_manifest)
            print('writing bundle to {}'.format(out_path))

        with zipfile.ZipFile(out_path, 'w') as z:
            for f in self.bundle_files:
                if isinstance(f, PebbleBundle):
                    for bf in f.bundle_files:
                        z.write(bf, os.path.join(f.subfolder, os.path.basename(bf)))
                    f.bundle_manifest['type'] = f.type
                    if f.rocky_info:
                        f.bundle_manifest['rocky'] = f.rocky_info
                    z.writestr(os.path.join(f.subfolder, 'manifest.json'), json.dumps(f.bundle_manifest))
                else:
                    z.write(f, os.path.basename(f))
                    

            if not self.has_children:
                self.bundle_manifest['type'] = self.type
                z.writestr('manifest.json', json.dumps(self.bundle_manifest))

        if verbose:
            print('done!')

def check_required_args(opts, *args):
    options = vars(opts)
    for required_arg in args:
        try:
            if not options[required_arg]:
                raise Exception("Missing argument {}".format(required_arg))
        except KeyError:
            raise Exception("Missing argument {}".format(required_arg))

def make_firmware_bundle(firmware,
                         firmware_timestamp,
                         firmware_commit,
                         firmware_type,
                         board,
                         firmware_version_tag,
                         resources=None,
                         resources_timestamp=None,
                         outfile=None,
                         verbose=False):
    bundle = PebbleBundle()

    firmware_path = os.path.expanduser(firmware)
    bundle.add_firmware(firmware_path, firmware_type, firmware_timestamp,
        firmware_commit, board, firmware_version_tag)

    if resources:
        resources_path = os.path.expanduser(args.resources)
        bundle.add_resources(resources_path, args.resources_timestamp)

    bundle.write(outfile, verbose)

def make_watchapp_bundle(timestamp,
                         appinfo,
                         binaries,
                         js,
                         outfile=None,
                         verbose=False):
    """ Makes a pbw for an watch app, which includes a firmware, a resource
    pack and optionally a list of javascript files.

    Keyword arguments
    timestamp -- bundle timestamp
    appinfo -- path to the appinfo.json for the watch app
    sdk_version -- version of the Pebble SDK used to build binaries
    binaries -- list of binaries built for Pebble platforms
        'watchapp' -- path to the watchapp binary file
        'resources' -- path to resource .pbpack
        'worker_bin' -- (optional) path to the worker binary file
        'sdk_version' -- version of SDK used to build binary
        'subfolder' -- path to subfolder in PBW for platform binary
    js -- (optional) a list of paths to javascript files to be included
    outfile -- path to write the pbw to
    """
    bundle = PebbleBundle()

    appinfo_path = os.path.expanduser(appinfo)
    bundle.add_appinfo(appinfo_path)

    rocky_files = {}

    for js_file in js:
        if js_file.endswith('rocky-app.js'):
            platform = os.path.dirname(os.path.relpath(js_file, 'build')).split('/', 1)[0]
            rocky_files[platform] = js_file
            js.remove(js_file)
            continue
    bundle.add_jsapp(js)

    if len(binaries) < 1:
        raise Exception("Cannot bundle watchapp without binaries")

    for binary in binaries:
        bundle.has_children = True
        platform_bundle = PebbleBundle(subfolder=binary['subfolder'])

        if rocky_files:
            rocky_file = rocky_files.get(platform_bundle.subfolder,
                                         rocky_files.get('resources', None))
            platform_bundle.add_rockyjs(rocky_file, bundle)

        if binary['watchapp']:
            watchapp_path = os.path.expanduser(binary['watchapp'])
            platform_bundle.add_watchapp(watchapp_path, timestamp, binary['sdk_version'])
            bundle.has_watchapp = True

        if binary['worker_bin']:
            worker_bin_path = os.path.expanduser(binary['worker_bin'])
            platform_bundle.add_worker(worker_bin_path, timestamp, binary['sdk_version'])
 
        if binary['resources']:
            resources_path = os.path.expanduser(binary['resources'])
            platform_bundle.add_resources(resources_path, timestamp, binary['sdk_version'])

        bundle.bundle_files.append(platform_bundle)
    
    bundle.write(outfile, verbose)

def cmd_firmware(args):
    make_firmware_bundle(**vars(args))

def cmd_watchapp(args):
    args.sdk_version = dict(zip(['major', 'minor'], [int(x) for x in args.sdk_version.split('.')]))

    make_watchapp_bundle(**vars(args))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Create a Pebble bundle.')

    subparsers = parser.add_subparsers(help='commands')

    firmware_parser = subparsers.add_parser('firmware', help='create a Pebble firmware bundle')
    firmware_parser.add_argument('--firmware', help='path to the firmware .bin')
    firmware_parser.add_argument('--firmware-timestamp', help='the (git) timestamp of the firmware', type=int)
    firmware_parser.add_argument('--firmware-type', help='the type of firmware included in the bundle', choices = ['normal', 'recovery'])
    firmware_parser.add_argument('--board', help='the board for which the firmware was built', choices = ['bigboard', 'ev1', 'ev2'])
    firmware_parser.add_argument('--firmware-version', help='the firmware version tag')
    firmware_parser.set_defaults(func=cmd_firmware)

    watchapp_parser = subparsers.add_parser('watchapp', help='create Pebble watchapp bundle')
    watchapp_parser.add_argument('--appinfo', help='path to appinfo.json')
    watchapp_parser.add_argument('--watchapp', help='path to the watchapp .bin')
    watchapp_parser.add_argument('--watchapp-timestamp', help='the (git) timestamp of the app', type=int)
    watchapp_parser.add_argument('--javascript', help='path to the directory with the javascript app files to include')
    watchapp_parser.add_argument('--sdk-version', help='the SDK platform version required to run the app', type=str)
    watchapp_parser.add_argument('--resources', help='path to the generated resource pack')
    watchapp_parser.add_argument('--resources-timestamp', help='the (git) timestamp of the resource pack', type=int)
    watchapp_parser.add_argument("-v", "--verbose", help="print additional output", action="store_true")
    watchapp_parser.add_argument("-o", "--outfile", help="path to the output file")
    watchapp_parser.set_defaults(func=cmd_watchapp)

    if len(sys.argv) <= 1:
        parser.print_help()
        sys.exit(1)

    args = parser.parse_args()
    parser_func = args.func
    del args.func
    parser_func(args)
