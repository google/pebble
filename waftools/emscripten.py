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

"""
    Waftool that wraps the 'cprogram' feature and extends it with
    Emscripten-specific variables, examples:

    bld.program(source=sources,
            target=target,
            emx_pre_js_files=[],             # list of nodes used with --pre-js
            emx_post_js_files=[],            # list of nodes used with --post-js
            emx_exported_functions=node, # node to use with EXPORTED_FUNCTIONS
            emx_other_settings=[],       # list of -s settings
            emx_embed_files=[pbpack],    # list of nodes used with --embed-file
    )

    Also adds these optional env variables:

    bld.env.EMX_PRE_JS_FILES = []
    bld.env.EMX_POST_JS_FILES = []
    bld.env.EMX_EXPORTED_FUNCTIONS = []
    bld.env.EMX_OTHER_SETTINGS = []
    bld.env.EMX_EMBED_FILES = []
    bld.env.EMCC_DEBUG = 2
    bld.env.EMCC_CORES = 1

"""

import os
from waflib import Logs, Task, TaskGen


# Insipred on https://github.com/waf-project/waf/blob/4ff5b8b7a74dd2ad23600ed7af6a505b90235387/playground/strip/strip.py
def wrap_cprogram_task_class():
    classname = 'cprogram'
    orig_cls = Task.classes[classname]
    emx_cls = type(classname, (orig_cls,), {
        'run_str': '${CC} ${CFLAGS} ${DEFINES_ST:DEFINES} ${CPPPATH_ST:INCPATHS} ${SRC} -o ${TGT[0].abspath()} ${STLIBPATH_ST:STLIBPATH} ${STLIB_ST:STLIB} ${EMCC_SETTINGS}',
        'ext_out': ['.js'],
        # *waf* env vars that affect the output and thus should trigger a rebuild.
        # This is by no means a complete list, but just the stuff we use today.
        'vars': [
            'EMCC_DEBUG',
            'EMCC_CORES',
            'EMCC_SETTINGS',
            'LINKDEPS'
        ],
        'color': 'ORANGE',
    })
    wrapper_cls = type(classname, (emx_cls,), {})

    def init(self, *k, **kw):
        Task.Task.__init__(self, *k, **kw)
        # Set relevant OS environment variables:
        self.env.env = {}
        self.env.env.update(os.environ)
        for key in ['EMCC_DEBUG', 'EMCC_CORES', 'EM_CACHE']:
            if self.env[key]:  # If not explicitly set, empty list is returned
                self.env.env[key] = str(self.env[key])
    emx_cls.__init__ = init

    def run(self):
        if self.env.CC != 'emcc':
            return orig_cls.run(self)
        return emx_cls.run(self)
    wrapper_cls.run = run


cache_primer_node_by_cache_path = {}


def add_cache_primer_node_if_needed(bld, env):
    def get_cache_path(env):
        return env.EM_CACHE if env.EM_CACHE else '~/.emscripten_cache'

    cache_path = get_cache_path(env)
    existing_primer_node = \
        cache_primer_node_by_cache_path.get(cache_path, None)
    if existing_primer_node:
        return existing_primer_node
    # Build a tiny "hello world" C program to prime the caches:
    primer_node = bld.path.get_bld().make_node('emscripten-cache-primer.js')
    source_node = bld.path.find_node('waftools/emscripten-cache-primer-main.c')
    primer_env = env.derive().detach()
    # Force -O2, this will cause optimizer.exe (part of cached files) to be
    # compiled:
    primer_env['CFLAGS'] = filter(
        lambda flag: flag not in ['-O0', '-O1', '-O3', '-Oz', '-Os'],
        primer_env['CFLAGS']
    )
    primer_env['CFLAGS'].append('-O2')
    bld.program(
        source=[source_node],
        target=primer_node,
        env=primer_env)
    cache_primer_node_by_cache_path[cache_path] = primer_node
    return primer_node


@TaskGen.feature('cprogram')
@TaskGen.after_method('apply_link')
def process_emscripten_cprogram_link_args(self):
    task = self.link_task
    task.env.EMCC_SETTINGS = []

    def add_emcc_settings(*args):
        for val_or_node in args:
            if isinstance(val_or_node, basestring):
                val = val_or_node
            else:
                val = val_or_node.abspath()
                task.dep_nodes.append(val_or_node)
            task.env.EMCC_SETTINGS.append(val)

    def get_rule_and_env_values(var_name):
        # Rule values first:
        vals = getattr(self, var_name.lower(), [])
        # Add env values to the end:
        vals.extend(getattr(self.env, var_name.upper()))
        return vals

    def get_single_rule_or_env_value(var_name):
        val = getattr(self, var_name.lower(), None)
        if val:
            return val
        return getattr(self.env, var_name.upper(), None)

    for node in get_rule_and_env_values('emx_pre_js_files'):
        add_emcc_settings('--pre-js', node)
    for node in get_rule_and_env_values('emx_post_js_files'):
        add_emcc_settings('--post-js', node)

    transform_js_node_and_args = \
        get_single_rule_or_env_value('emx_transform_js_node_and_args')
    if transform_js_node_and_args:
        add_emcc_settings('--js-transform', *transform_js_node_and_args)

    exported_functions = get_single_rule_or_env_value('emx_exported_functions')
    if exported_functions:
        exported_func_path = self.emx_exported_functions.abspath()
        add_emcc_settings(
            '-s', 'EXPORTED_FUNCTIONS=@{}'.format(exported_func_path))
        task.dep_nodes.append(self.emx_exported_functions)

    for node in get_rule_and_env_values('emx_embed_files'):
        add_emcc_settings('--embed-file', node)

    for s in get_rule_and_env_values('emx_other_settings'):
        add_emcc_settings('-s', s)

    # Emscripten implicitly regenerates caches (libc.bc, dlmalloc.bc,
    # struct_info.compiled.json and optimizer.exe) as needed.
    # When running multiple instantiations of emcc in parallel, this is
    # problematic because they will each race to generate the caches,
    # using the same locations for writing/using/executing them.
    # See also https://github.com/kripken/emscripten/issues/4151

    if self.env.CC == 'emcc':
        primer_node = add_cache_primer_node_if_needed(self.bld, self.env)
        if task.outputs[0] != primer_node:
            task.dep_nodes.append(primer_node)


def build(bld):
    wrap_cprogram_task_class()


def configure(conf):
    conf.find_program('emcc', errmsg='emscripten is not installed')
    conf.find_program('em-config', var='EM-CONFIG')

    conf.env.EMSCRIPTEN_ROOT = conf.cmd_and_log([
      'em-config', 'EMSCRIPTEN_ROOT'
    ]).rstrip()
    Logs.pprint(
      'YELLOW', 'Emscripten path is {}'.format(conf.env.EMSCRIPTEN_ROOT))
    conf.env.CC = 'emcc'
    conf.env.LINK_CC = 'emcc'
    conf.env.AR = 'emar'

    # Don't look at the host system headers:
    conf.env.CFLAGS.extend([
      '-nostdinc',
      '-Xclang', '-nobuiltininc',
      '-Xclang', '-nostdsysteminc'
    ])
