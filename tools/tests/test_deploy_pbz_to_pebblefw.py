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

import json
import mock
import nose
import os
import sys
import unittest

# Allow us to run even if not at the `tools` directory.
root_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), os.pardir))
sys.path.insert(0, root_dir)

import deploy_pbz_to_pebblefw


class TestDeployPbzToPebbleFw(unittest.TestCase):
    DUMMY_MANIFEST_CONTENT = {
        'versionTag': 'v3.1',
        'timestamp': 1377981535,
        'hwrev': 'bb2',
        'type': 'normal'
    }

    @mock.patch('boto.connect_s3')
    @mock.patch('boto.s3')
    @mock.patch('deploy_pbz_to_pebblefw.open')
    @mock.patch('deploy_pbz_to_pebblefw._sha256_file')
    @mock.patch('requests.get')
    @mock.patch('libpebble2.util.bundle.PebbleBundle')
    def test_no_latest_exists_dry_run(self, mock_pebble_bundle, mock_requests_get, mock_sha256_file,
                                      mock_open, mock_boto_s3, mock_boto_connect_s3):

        # Set up our mocks
        mock_pebble_bundle_instance = mock_pebble_bundle.return_value
        mock_pebble_bundle_instance.get_manifest.return_value = {
            'firmware': self.DUMMY_MANIFEST_CONTENT
        }

        dummy_sha = '620cfcadc8d28240048ffa01eb6984b06774a584349f178564e1548ecc813903'
        mock_sha256_file.return_value = dummy_sha

        mock_requests_get.return_value.status_code = 403

        dummy_notes = 'Dummy notes'
        mock.mock_open(mock_open, read_data=dummy_notes)

        # Run the code under test
        dummy_pbz_path = 'dummy_pbz.pbz'
        dummy_notes_path = 'dummy_notes.txt'
        deploy_pbz_to_pebblefw.deploy_bundle(dummy_pbz_path, 'pebblefw-staging', 'porksmoothie',
                                             dummy_notes_path, dry_run=True)

        # Check out mocks to make sure everything worked
        mock_sha256_file.assert_called_with(dummy_pbz_path)

        mock_open.assert_called_with(dummy_notes_path, 'r')

        mock_requests_get.assert_called_with(
                'https://pebblefw-staging.s3.amazonaws.com/pebble/bb2/porksmoothie/latest.json')

        # We're using dry_run=True, we better not talk to s3
        assert not mock_boto_connect_s3.called

    @mock.patch('boto.connect_s3')
    @mock.patch('boto.s3')
    @mock.patch('deploy_pbz_to_pebblefw.open')
    @mock.patch('deploy_pbz_to_pebblefw._sha256_file')
    @mock.patch('requests.get')
    @mock.patch('libpebble2.util.bundle.PebbleBundle')
    def test_no_latest_exists_no_dry_run(self, mock_pebble_bundle, mock_requests_get,
                                         mock_sha256_file, mock_open, mock_boto_s3,
                                         mock_boto_connect_s3):

        # Set up our mocks
        mock_pebble_bundle_instance = mock_pebble_bundle.return_value
        mock_pebble_bundle_instance.get_manifest.return_value = {
            'firmware': self.DUMMY_MANIFEST_CONTENT
        }

        dummy_sha = '620cfcadc8d28240048ffa01eb6984b06774a584349f178564e1548ecc813903'
        mock_sha256_file.return_value = dummy_sha

        mock_requests_get.return_value.status_code = 403

        dummy_notes = 'Dummy notes'
        mock.mock_open(mock_open, read_data=dummy_notes)

        mock_latest_key = mock.MagicMock()

        def boto_key_func(boto_bucket, key):
            if key == 'pebble/bb2/porksmoothie/latest.json':
                # Only validate us uploading to latest.json and return unnamed mocks for the other
                # paths.
                return mock_latest_key

            return mock.DEFAULT

        mock_boto_s3.key.Key.side_effect = boto_key_func

        # Run the code under test
        dummy_pbz_path = 'dummy_pbz.pbz'
        dummy_notes_path = 'dummy_notes.txt'
        deploy_pbz_to_pebblefw.deploy_bundle(dummy_pbz_path, 'pebblefw-staging', 'porksmoothie',
                                             dummy_notes_path)

        # Check out mocks to make sure everything worked
        mock_sha256_file.assert_called_with(dummy_pbz_path)

        mock_open.assert_called_with(dummy_notes_path, 'r')

        mock_requests_get.assert_called_with(
                'https://pebblefw-staging.s3.amazonaws.com/pebble/bb2/porksmoothie/latest.json')

        assert mock_boto_connect_s3.called
        expected_new_latest_json = {
            'normal': {
                'url': 'https://pebblefw-staging.s3.amazonaws.com/pebble/bb2/porksmoothie/pbz/' +
                       os.path.basename(dummy_pbz_path),
                'timestamp': self.DUMMY_MANIFEST_CONTENT['timestamp'],
                'notes': dummy_notes,
                'friendlyVersion': self.DUMMY_MANIFEST_CONTENT['versionTag'],
                'sha-256': dummy_sha
            }
        }
        actual_new_latest_json = mock_latest_key.set_contents_from_string.call_args[0][0]

        # Send it through the json loader to normalize any formatting
        actual_new_latest_json = json.dumps(json.loads(actual_new_latest_json))

        nose.tools.eq_(json.dumps(expected_new_latest_json), actual_new_latest_json)
