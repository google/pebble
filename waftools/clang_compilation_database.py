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

# encoding: utf-8
# Christoph Koke, 2013

"""Writes the c and cpp compile commands into build/compile_commands.json
see http://clang.llvm.org/docs/JSONCompilationDatabase.html"""

import json
import os
from waflib import Logs, TaskGen, Task
from waflib.Tools import c, cxx

@TaskGen.feature('*')
@TaskGen.after_method('process_use')
def collect_compilation_db_tasks(self):
        "Add a compilation database entry for compiled tasks"
        try:
                clang_db = self.bld.clang_compilation_database_tasks
        except AttributeError:
                clang_db = self.bld.clang_compilation_database_tasks = []
                self.bld.add_post_fun(write_compilation_database)

        for task in getattr(self, 'compiled_tasks', []):
                if isinstance(task, (c.c, cxx.cxx)):
                        clang_db.append(task)


def write_compilation_database(ctx):
        "Write the clang compilation database as json"
        database_file = ctx.bldnode.make_node('compile_commands.json')
        file_path = str(database_file.path_from(ctx.path))
        if not os.path.exists(file_path):
            with open(file_path, 'w') as f:
                f.write('[]')
        Logs.info("Store compile commands in %s" % file_path)
        clang_db = dict((x["file"], x) for x in json.load(database_file))
        for task in getattr(ctx, 'clang_compilation_database_tasks', []):
                try:
                        cmd = task.last_cmd
                except AttributeError:
                        continue
                filename = task.inputs[0].abspath()
                entry = {
                        "directory" : getattr(task, 'cwd', ctx.variant_dir),
                        "command"   : " ".join(cmd),
                        "file"    : filename,
                }
                clang_db[filename] = entry
        database_file.write(json.dumps(clang_db.values(), indent=2))


def options(opt):
        "options for clang_compilation_database"
        pass


def configure(cfg):
        "configure for clang_compilation_database"
        pass
