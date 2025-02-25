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

import argparse
import boto
import hashlib
import json
import os
import requests

import libpebble2.util.bundle

S3_BUCKETS = ['pebblefw', 'pebblefw-staging']
STAGES = ['release', 'release-v2', 'release-v3', 'release-v3.7', 'release-v3.8',
          'test', 'beta', 'nightly', 'porksmoothie', 'codshake', 'anchovietea']


def _sha256_file(path):
    with open(path, 'rb') as f:
        data = f.read()
        return hashlib.sha256(data).hexdigest()


def _build_s3_path(*args):
    return 'pebble/' + '/'.join(args)


def _build_s3_url(bucket, path):
    return 'https://{}.s3.amazonaws.com/{}'.format(bucket, path)


def _upload_to_s3(boto_bucket, key, filename=None, content=None, headers=None):
    """ Need to set either filename or content """

    boto_key = boto.s3.key.Key(boto_bucket, key)

    if filename is not None:
        print 'PUT %s filename=%s' % (key, filename)
        boto_key.set_contents_from_filename(filename, headers=headers)
    elif content is not None:
        print 'PUT %s string=<%s>' % (key, content)
        boto_key.set_contents_from_string(content, headers=headers)
    else:
        raise Exception('Need to specify either filename or content')

    boto_key.set_acl('public-read')


def deploy_bundle(bundle_path, bucket, stage, notes_path, layouts_path=None, dry_run=False):
    # Parse the bundle to deploy
    bundle = libpebble2.util.bundle.PebbleBundle(bundle_path)

    bundle_manifest = bundle.get_manifest()

    friendly_version = bundle_manifest['firmware']['versionTag']

    # Generate a deployment manifest
    deploy_manifest = {
        'friendlyVersion': friendly_version,
        'timestamp': bundle_manifest['firmware']['timestamp'],
        'sha-256': _sha256_file(bundle_path),
    }

    bundle_filename = os.path.basename(bundle_path)
    board = bundle_manifest['firmware']['hwrev']

    bundle_key = _build_s3_path(board, stage, 'pbz', bundle_filename)
    deploy_manifest['url'] = _build_s3_url(bucket, bundle_key)

    if layouts_path is not None:
        layouts_key = _build_s3_path(board, stage, 'layouts', friendly_version + '_layouts.json')
        deploy_manifest['layouts'] = _build_s3_url(bucket, layouts_key)

    with open(notes_path, 'r') as f:
        deploy_manifest['notes'] = f.read().strip()

    # Fetch the current latest.json
    latest_key = _build_s3_path(board, stage, 'latest.json')
    r = requests.get(_build_s3_url(bucket, latest_key))
    if r.status_code == 403:
        # Starting from scratch
        latest_json = {}
    else:
        latest_json = r.json()

    # Update the latest.json with the new deployment
    firmware_type = bundle_manifest['firmware']['type']
    latest_json[firmware_type] = deploy_manifest

    if dry_run:
        # We're done!
        print "Deploying {} to bucket: {} board: {} stage: {} version: {}".format(
            bundle_path, bucket, board, stage, friendly_version)

        print '== Before =='
        if r.status_code == 403:
            print "No latest.json exists"
        else:
            print json.dumps(r.json(), indent=2)

        print '== After =='
        print json.dumps(latest_json, indent=2)

        return

    # Upload the stuff!
    boto_conn = boto.connect_s3()
    boto_bucket = boto.s3.bucket.Bucket(boto_conn, bucket)

    _upload_to_s3(boto_bucket, bundle_key, filename=bundle_path)

    if layouts_path is not None:
        _upload_to_s3(boto_bucket, layouts_key, filename=layouts_path)

    # prevent caching of the latest.json file
    headers = {'Cache-Control': 'max-age=0'}
    _upload_to_s3(boto_bucket, latest_key, content=json.dumps(latest_json), headers=headers)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('bundle')
    parser.add_argument('--bucket', choices=S3_BUCKETS, required=True)
    parser.add_argument('--stage', choices=STAGES, required=True)
    parser.add_argument('--notes', required=True, help='path to very brief release notes')
    parser.add_argument('--layouts', default=None, required=False, help='path to the layouts. json')
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--debug', action='store_true', help='print some very verbose logs')

    args = parser.parse_args()

    if args.debug:
        import logging
        logging.basicConfig(level=logging.DEBUG)

    deploy_bundle(args.bundle, args.bucket, args.stage, args.notes, args.layouts, args.dry_run)

if (__name__ == '__main__'):
    main()
