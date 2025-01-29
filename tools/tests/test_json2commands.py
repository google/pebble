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
import array
from struct import pack

from generate_pdcs import json2commands, pebble_commands

class MyTestCase(unittest.TestCase):


    def test_parse_color(self):
        truncate = True

        # Test valid values
        color_opacity = [0, 0.333, 0.666, 1]
        color = json2commands.parse_color(color_opacity, truncate)
        self.assertEqual(color, pebble_commands.convert_color(0, 85, 170, 255, truncate))

        # Test invalid values
        color_opacity = [0, 0.333, 0.666, 2]
        color = json2commands.parse_color(color_opacity, truncate)
        self.assertEqual(color, 0)

        color_opacity = [0, 0.333, 0.666, -2]
        color = json2commands.parse_color(color_opacity, truncate)
        self.assertEqual(color, 0)

    def test_parse_json_line_data(self):
        # Test skipping of invisible segments
        json_line_data = [{
            "startPoint":   [json2commands.INVISIBLE_POINT_THRESHOLD + 1, 1.0],
            "endPoint":     [2.0, 1.0]
        },{
           "startPoint":    [1.0, 1.0],
            "endPoint":     [2.0, 1.0] 
        }]
        bidirectional_lines = json2commands.parse_json_line_data(json_line_data)
        self.assertEqual(bidirectional_lines, [ ((1.0, 1.0), (2.0, 1.0)),
                                                ((2.0, 1.0), (1.0, 1.0))])

        json_line_data = [{
            "startPoint":   [2.0, 1.0],
            "endPoint":     [2.0, -json2commands.INVISIBLE_POINT_THRESHOLD - 1]
        }]
        bidirectional_lines = json2commands.parse_json_line_data(json_line_data)
        self.assertEqual(bidirectional_lines, [])

        # Test skipping of duplicate segments
        json_line_data = [{
            "startPoint":   [1.0, 1.0],
            "endPoint":     [2.0, 1.0]
        },{
           "startPoint":    [2.0, 1.0],
            "endPoint":     [1.0, 1.0] 
        },{
           "startPoint":    [1.0, 1.0],
            "endPoint":     [2.0, 1.0] 
        }]
        bidirectional_lines = json2commands.parse_json_line_data(json_line_data)
        self.assertEqual(bidirectional_lines, [ ((1.0, 1.0), (2.0, 1.0)),
                                                ((2.0, 1.0), (1.0, 1.0))])

    def test_determine_longest_path(self):
        # Test point
        json_line_data = [{
            "startPoint":   [1.0, 1.0],
            "endPoint":     [1.0, 1.0]
        }]
        bidirectional_lines = json2commands.parse_json_line_data(json_line_data)

        longest_path = json2commands.determine_longest_path(bidirectional_lines)
        self.assertEqual(longest_path, [(1.0, 1.0), (1.0, 1.0)])
        self.assertEqual(len(bidirectional_lines), 0)

        # Test simplest line segment
        json_line_data = [{
            "startPoint":   [1.0, 1.0],
            "endPoint":     [2.0, 1.0]
        }]
        bidirectional_lines = json2commands.parse_json_line_data(json_line_data)

        longest_path = json2commands.determine_longest_path(bidirectional_lines)
        self.assertEqual(longest_path, [(1.0, 1.0), (2.0, 1.0)])
        self.assertEqual(len(bidirectional_lines), 0)

        # Test ordered connected line segments
        json_line_data = [{
            "startPoint":   [1.0, 1.0],
            "endPoint":     [2.0, 1.0]
        },{
            "startPoint":   [2.0, 1.0],
            "endPoint":     [2.0, 2.0]
        }]
        bidirectional_lines = json2commands.parse_json_line_data(json_line_data)

        longest_path = json2commands.determine_longest_path(bidirectional_lines)
        self.assertEqual(longest_path, [(1.0, 1.0), (2.0, 1.0), (2.0, 2.0)])
        self.assertEqual(len(bidirectional_lines), 0)

        # Test unordered connected acyclic line segments
        json_line_data = [{
            "startPoint":   [1.0, 1.0],
            "endPoint":     [2.0, 1.0]
        },{
            "startPoint":   [2.0, 2.0],
            "endPoint":     [2.0, 1.0]
        }]
        bidirectional_lines = json2commands.parse_json_line_data(json_line_data)

        longest_path = json2commands.determine_longest_path(bidirectional_lines)
        self.assertEqual(longest_path, [(1.0, 1.0), (2.0, 1.0), (2.0, 2.0)])
        self.assertEqual(len(bidirectional_lines), 0)

        # Test connected simplest cyclic segments (circle)
        json_line_data = [{
            "startPoint":   [1.0, 1.0],
            "endPoint":     [2.0, 1.0]
        },{
            "startPoint":   [2.0, 1.0],
            "endPoint":     [2.0, 2.0]
        },{
            "startPoint":   [2.0, 2.0],
            "endPoint":     [1.0, 1.0]
        }]
        bidirectional_lines = json2commands.parse_json_line_data(json_line_data)

        longest_path = json2commands.determine_longest_path(bidirectional_lines)
        self.assertEqual(longest_path, [(1.0, 1.0), (2.0, 1.0), (2.0, 2.0), (1.0, 1.0)])
        self.assertEqual(len(bidirectional_lines), 0)

        # Test connected complex cyclic line segments 
        json_line_data = [{
            "startPoint":   [1.0, 1.0],
            "endPoint":     [2.0, 1.0]
        },{
            "startPoint":   [2.0, 1.0],
            "endPoint":     [2.0, 2.0]
        },{
            "startPoint":   [2.0, 2.0],
            "endPoint":     [1.0, 1.0]
        },{
            "startPoint":   [1.0, 2.0],
            "endPoint":     [1.0, 1.0]
        }]
        bidirectional_lines = json2commands.parse_json_line_data(json_line_data)

        longest_path = json2commands.determine_longest_path(bidirectional_lines)
        self.assertEqual(longest_path, [(1.0, 2.0), (1.0, 1.0), (2.0, 1.0), (2.0, 2.0), (1.0, 1.0)])
        self.assertEqual(len(bidirectional_lines), 0)

        # Test connected segments with more than one path
        json_line_data = [{
            "startPoint":   [1.0, 1.0],
            "endPoint":     [2.0, 1.0]
        },{
            "startPoint":   [2.0, 2.0],
            "endPoint":     [2.0, 1.0]
        },{
            "startPoint":   [3.0, 1.0],
            "endPoint":     [2.0, 1.0]
        }]
        bidirectional_lines = json2commands.parse_json_line_data(json_line_data)

        longest_path = json2commands.determine_longest_path(bidirectional_lines)
        self.assertEqual(longest_path, [(3.0, 1.0), (2.0, 1.0), (1.0, 1.0)])
        self.assertEqual(bidirectional_lines, [((2.0, 2.0), (2.0, 1.0)), ((2.0, 1.0), (2.0, 2.0))])

        longest_path = json2commands.determine_longest_path(bidirectional_lines)
        self.assertEqual(longest_path, [(2.0, 1.0), (2.0, 2.0)])
        self.assertEqual(len(bidirectional_lines), 0)

        # Test (ordered) unconnected segments (implicitly more than one path)
        json_line_data = [{
            "startPoint":   [1.0, 1.0],
            "endPoint":     [2.0, 1.0]
        },{
            "startPoint":   [1.0, 2.0],
            "endPoint":     [2.0, 2.0]
        }]
        bidirectional_lines = json2commands.parse_json_line_data(json_line_data)

        longest_path = json2commands.determine_longest_path(bidirectional_lines)
        self.assertEqual(longest_path, [(1.0, 2.0), (2.0, 2.0)])
        self.assertEqual(bidirectional_lines, [((1.0, 1.0), (2.0, 1.0)), ((2.0, 1.0), (1.0, 1.0))])

        longest_path = json2commands.determine_longest_path(bidirectional_lines)
        self.assertEqual(longest_path, [(1.0, 1.0), (2.0, 1.0)])
        self.assertEqual(len(bidirectional_lines), 0)

    def test_process_fill(self):
        # Test that line style is taken from first segment
        fillGroup_data = [{
            "thickness":    3,
            "color":        [0, 0, 0, 1],
            "startPoint":   [1.0, 1.0],
            "endPoint":     [2.0, 1.0]
        },{
            "thickness":    3,
            "color":        [0, 0, 0.666, 0.333],
            "startPoint":   [2.0, 1.0],
            "endPoint":     [2.0, 2.0]
        },{
            "thickness":    3,
            "color":        [0.666, 0.333, 0, 0],
            "startPoint":   [2.0, 2.0],
            "endPoint":     [1.0, 1.0]
        }]
        truncate_color = True
        fill_commands, error = json2commands.process_fill(
                                                    fillGroup_data,
                                                    (0, 0),
                                                    (json2commands.DISPLAY_DIM_X, json2commands.DISPLAY_DIM_Y),
                                                    False,
                                                    False,
                                                    False,
                                                    truncate_color)
        self.assertFalse(error)
        fill_command = fill_commands[0]
        self.assertIsInstance(fill_command, pebble_commands.PathCommand)
        self.assertEqual(len(fill_command.points), 4)
        self.assertEqual(fill_command.fill_color, json2commands.parse_color([1, 1, 1, 1], truncate_color))
        self.assertEqual(fill_command.stroke_color, json2commands.parse_color([0, 0, 0, 1], truncate_color))
        self.assertEqual(fill_command.stroke_width, 3)
        self.assertTrue(pebble_commands.compare_points(fill_command.points[0], (1.0, 1.0)))
        self.assertTrue(pebble_commands.compare_points(fill_command.points[1], (2.0, 1.0)))
        self.assertTrue(pebble_commands.compare_points(fill_command.points[2], (2.0, 2.0)))
        self.assertTrue(pebble_commands.compare_points(fill_command.points[3], (1.0, 1.0)))
        self.assertFalse(fill_command.open)

        # Test that fill with no stroke width has no stroke color
        fillGroup_data = [{
            "thickness":    0,
            "color":        [0.666, 0, 0.666, 1],
            "startPoint":   [1.0, 1.0],
            "endPoint":     [2.0, 1.0]
        },{
            "thickness":    3,
            "color":        [0, 0, 0, 1],
            "startPoint":   [2.0, 1.0],
            "endPoint":     [2.0, 2.0]
        },{
            "thickness":    3,
            "color":        [0, 0, 0, 1],
            "startPoint":   [2.0, 2.0],
            "endPoint":     [1.0, 1.0]
        }]
        truncate_color = True
        fill_commands, error = json2commands.process_fill(
                                                    fillGroup_data,
                                                    (0, 0),
                                                    (json2commands.DISPLAY_DIM_X, json2commands.DISPLAY_DIM_Y),
                                                    False,
                                                    False,
                                                    False,
                                                    truncate_color)
        self.assertFalse(error)
        fill_command = fill_commands[0]
        self.assertIsInstance(fill_command, pebble_commands.PathCommand)
        self.assertEqual(len(fill_command.points), 4)
        self.assertEqual(fill_command.fill_color, json2commands.parse_color([1, 1, 1, 1], truncate_color))
        self.assertEqual(fill_command.stroke_color, 0)
        self.assertEqual(fill_command.stroke_width, 0)
        self.assertTrue(pebble_commands.compare_points(fill_command.points[0], (1.0, 1.0)))
        self.assertTrue(pebble_commands.compare_points(fill_command.points[1], (2.0, 1.0)))
        self.assertTrue(pebble_commands.compare_points(fill_command.points[2], (2.0, 2.0)))
        self.assertTrue(pebble_commands.compare_points(fill_command.points[3], (1.0, 1.0)))
        self.assertFalse(fill_command.open)

        # Test that fill with no stroke color has no stroke width
        fillGroup_data = [{
            "thickness":    3,
            "color":        [0, 1, 0.666, 0],
            "startPoint":   [1.0, 1.0],
            "endPoint":     [2.0, 1.0]
        },{
            "thickness":    3,
            "color":        [0, 0, 0, 1],
            "startPoint":   [2.0, 1.0],
            "endPoint":     [2.0, 2.0]
        },{
            "thickness":    3,
            "color":        [0, 0, 0, 1],
            "startPoint":   [2.0, 2.0],
            "endPoint":     [1.0, 1.0]
        }]
        truncate_color = True
        fill_commands, error = json2commands.process_fill(
                                                    fillGroup_data,
                                                    (0, 0),
                                                    (json2commands.DISPLAY_DIM_X, json2commands.DISPLAY_DIM_Y),
                                                    False,
                                                    False,
                                                    False,
                                                    truncate_color)
        self.assertFalse(error)
        fill_command = fill_commands[0]
        self.assertIsInstance(fill_command, pebble_commands.PathCommand)
        self.assertEqual(len(fill_command.points), 4)
        self.assertEqual(fill_command.fill_color, json2commands.parse_color([1, 1, 1, 1], truncate_color))
        self.assertEqual(fill_command.stroke_color, 0)
        self.assertEqual(fill_command.stroke_width, 0)
        self.assertTrue(pebble_commands.compare_points(fill_command.points[0], (1.0, 1.0)))
        self.assertTrue(pebble_commands.compare_points(fill_command.points[1], (2.0, 1.0)))
        self.assertTrue(pebble_commands.compare_points(fill_command.points[2], (2.0, 2.0)))
        self.assertTrue(pebble_commands.compare_points(fill_command.points[3], (1.0, 1.0)))
        self.assertFalse(fill_command.open)

    def test_process_open_paths(self):
        # Test group of varying stroke width and stroke color.
        fillGroup_data = [{
            "thickness":    3,
            "color":        [0, 0, 0, 1], # 192
            "startPoint":   [1.0, 1.0],
            "endPoint":     [2.0, 2.0]
        },{
            "thickness":    5,
            "color":        [0, 0, 0.333, 0.333], # 65
            "startPoint":   [3.0, 3.0],
            "endPoint":     [4.0, 4.0]
        },{
            "thickness":    5,
            "color":        [0, 0.666, 0, 0.333], # 72
            "startPoint":   [4.0, 4.0],
            "endPoint":     [4.0, 5.0]
        },{
            "thickness":    3,
            "color":        [0, 0, 0.666, 0.333], # 66
            "startPoint":   [2.0, 1.0],
            "endPoint":     [2.0, 2.0]
        },{
            "thickness":    2,
            "color":        [0.666, 0.333, 0, 1], # 228
            "startPoint":   [10.0, 10.0],
            "endPoint":     [11.0, 11.0]
        },{
            "thickness":    3,
            "color":        [0, 0, 0, 1], # 192
            "startPoint":   [1.0, 2.0],
            "endPoint":     [2.0, 2.0]
        }]
        truncate_color = True
        open_path_commands, error = json2commands.process_open_paths(
                                                        fillGroup_data,
                                                        (0, 0),
                                                        (json2commands.DISPLAY_DIM_X, json2commands.DISPLAY_DIM_Y),
                                                        True,
                                                        False,
                                                        False,
                                                        truncate_color)
        self.assertFalse(error)
        self.assertEqual(len(open_path_commands), 5)

        width_2_color_228_command = open_path_commands[0]
        self.assertIsInstance(width_2_color_228_command, pebble_commands.PathCommand)
        self.assertEqual(width_2_color_228_command.stroke_width, 2)
        self.assertEqual(width_2_color_228_command.stroke_color, json2commands.parse_color([0.666, 0.333, 0, 1],
            truncate_color))
        self.assertEqual(len(width_2_color_228_command.points), 2)
        self.assertEqual(width_2_color_228_command.fill_color, json2commands.parse_color([0, 0, 0, 0],
            truncate_color))
        self.assertTrue(pebble_commands.compare_points(width_2_color_228_command.points[0], (11.0, 11.0)))
        self.assertTrue(pebble_commands.compare_points(width_2_color_228_command.points[1], (10.0, 10.0)))
        self.assertTrue(width_2_color_228_command.open)

        width_3_color_192_command = open_path_commands[1]
        self.assertIsInstance(width_3_color_192_command, pebble_commands.PathCommand)
        self.assertEqual(width_3_color_192_command.stroke_width, 3)
        self.assertEqual(width_3_color_192_command.stroke_color, json2commands.parse_color([0, 0, 0, 1],
            truncate_color))
        self.assertEqual(len(width_3_color_192_command.points), 3)
        self.assertEqual(width_3_color_192_command.fill_color, json2commands.parse_color([0, 0, 0, 0],
            truncate_color))
        self.assertTrue(pebble_commands.compare_points(width_3_color_192_command.points[0], (1.0, 2.0)))
        self.assertTrue(pebble_commands.compare_points(width_3_color_192_command.points[1], (2.0, 2.0)))
        self.assertTrue(pebble_commands.compare_points(width_3_color_192_command.points[2], (1.0, 1.0)))
        self.assertTrue(width_3_color_192_command.open)

        width_3_color_66_command = open_path_commands[2]
        self.assertIsInstance(width_3_color_66_command, pebble_commands.PathCommand)
        self.assertEqual(width_3_color_66_command.stroke_width, 3)
        self.assertEqual(width_3_color_66_command.stroke_color, json2commands.parse_color([0, 0, 0.666, 0.333],
            truncate_color))
        self.assertEqual(len(width_3_color_66_command.points), 2)
        self.assertEqual(width_3_color_66_command.fill_color, json2commands.parse_color([0, 0, 0, 0],
            truncate_color))
        self.assertTrue(pebble_commands.compare_points(width_3_color_66_command.points[0], (2.0, 1.0)))
        self.assertTrue(pebble_commands.compare_points(width_3_color_66_command.points[1], (2.0, 2.0)))
        self.assertTrue(width_3_color_66_command.open)

        width_5_color_65_command = open_path_commands[3]
        self.assertIsInstance(width_5_color_65_command, pebble_commands.PathCommand)
        self.assertEqual(width_5_color_65_command.stroke_width, 5)
        self.assertEqual(width_5_color_65_command.stroke_color, json2commands.parse_color([0, 0, 0.333, 0.333],
            truncate_color))
        self.assertEqual(len(width_5_color_65_command.points), 2)
        self.assertEqual(width_5_color_65_command.fill_color, json2commands.parse_color([0, 0, 0, 0],
            truncate_color))
        self.assertTrue(pebble_commands.compare_points(width_5_color_65_command.points[0], (4.0, 4.0)))
        self.assertTrue(pebble_commands.compare_points(width_5_color_65_command.points[1], (3.0, 3.0)))
        self.assertTrue(width_5_color_65_command.open)

        width_5_color_72_command = open_path_commands[4]
        self.assertIsInstance(width_5_color_72_command, pebble_commands.PathCommand)
        self.assertEqual(width_5_color_72_command.stroke_width, 5)
        self.assertEqual(width_5_color_72_command.stroke_color, json2commands.parse_color([0, 0.666, 0, 0.333],
            truncate_color))
        self.assertEqual(len(width_5_color_72_command.points), 2)
        self.assertEqual(width_5_color_72_command.fill_color, json2commands.parse_color([0, 0, 0, 0],
            truncate_color))
        self.assertTrue(pebble_commands.compare_points(width_5_color_72_command.points[0], (4.0, 5.0)))
        self.assertTrue(pebble_commands.compare_points(width_5_color_72_command.points[1], (4.0, 4.0)))
        self.assertTrue(width_5_color_72_command.open)

        # Test that open with no stroke width has no stroke color
        fillGroup_data = [{
            "thickness":    0,
            "color":        [0.666, 0, 0.666, 1],
            "startPoint":   [1.0, 1.0],
            "endPoint":     [2.0, 1.0]
        }]
        truncate_color = True
        open_path_commands, error = json2commands.process_open_paths(
                                                        fillGroup_data,
                                                        (0, 0),
                                                        (json2commands.DISPLAY_DIM_X, json2commands.DISPLAY_DIM_Y),
                                                        True,
                                                        False,
                                                        False,
                                                        truncate_color)
        self.assertFalse(error)
        open_path_command = open_path_commands[0]
        self.assertIsInstance(open_path_command, pebble_commands.PathCommand)
        self.assertEqual(len(open_path_command.points), 2)
        self.assertEqual(open_path_command.fill_color, json2commands.parse_color([0, 0, 0, 0], truncate_color))
        self.assertEqual(open_path_command.stroke_color, 0)
        self.assertEqual(open_path_command.stroke_width, 0)
        self.assertTrue(pebble_commands.compare_points(open_path_command.points[0], (1.0, 1.0)))
        self.assertTrue(pebble_commands.compare_points(open_path_command.points[1], (2.0, 1.0)))
        self.assertTrue(open_path_command.open)

        # Test that open with no stroke color has no stroke width
        fillGroup_data = [{
            "thickness":    3,
            "color":        [0, 1, 0.666, 0],
            "startPoint":   [1.0, 1.0],
            "endPoint":     [2.0, 1.0]
        }]
        truncate_color = True
        open_path_commands, error = json2commands.process_open_paths(
                                                        fillGroup_data,
                                                        (0, 0),
                                                        (json2commands.DISPLAY_DIM_X, json2commands.DISPLAY_DIM_Y),
                                                        True,
                                                        False,
                                                        False,
                                                        truncate_color)
        self.assertFalse(error)
        open_path_command = open_path_commands[0]
        self.assertIsInstance(open_path_command, pebble_commands.PathCommand)
        self.assertEqual(len(open_path_command.points), 2)
        self.assertEqual(open_path_command.fill_color, json2commands.parse_color([0, 0, 0, 0], truncate_color))
        self.assertEqual(open_path_command.stroke_color, 0)
        self.assertEqual(open_path_command.stroke_width, 0)
        self.assertTrue(pebble_commands.compare_points(open_path_command.points[0], (1.0, 1.0)))
        self.assertTrue(pebble_commands.compare_points(open_path_command.points[1], (2.0, 1.0)))
        self.assertTrue(open_path_command.open)

    def test_parse_json_sequence(self):
        # Test mix of fills and open paths with multiple frames
        current_path = os.path.dirname(os.path.realpath(__file__))
        filename = current_path + '/json2commands_test.json'
        
        frames, errors, frame_duration = json2commands.parse_json_sequence(filename, (80, 80), False, False)
        self.assertEqual(frame_duration, 28)
        self.assertEqual(len(frames), 2)
        self.assertEqual(len(errors), 0)

        frame_1 = frames[0]
        self.assertEqual(len(frame_1), 4)
        self.assertFalse(frame_1[0].open)
        self.assertFalse(frame_1[1].open)
        self.assertTrue(frame_1[2].open)
        self.assertTrue(frame_1[3].open)

        frame_2 = frames[1]
        self.assertEqual(len(frame_2), 2)
        self.assertFalse(frame_2[0].open)
        self.assertTrue(frame_2[1].open)

if __name__ == '__main__':
    unittest.main()
