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
import time
import types

from waflib import Logs
from waflib.Configure import conf
from waflib.Task import Task
from waflib.TaskGen import after_method, before_method, feature
from waflib.Tools import c, c_preproc

import ldscript, process_bundle, process_headers, process_js, report_memory_usage, xcode_pebble
from pebble_sdk_platform import maybe_import_internal
from sdk_helpers import (append_to_attr, find_sdk_component, get_node_from_abspath,
                         wrap_task_name_with_platform)


# Override the default waf task __str__ method to include display of the HW platform being targeted
Task.__str__ = wrap_task_name_with_platform


def options(opt):
    """
    Specify the options available when invoking waf; uses OptParse. This method is called from
    app and lib waftools by `opt.load('pebble_sdk_common')`
    :param opt: the OptionContext object
    :return: N/A
    """
    opt.load('gcc')
    opt.add_option('-d', '--debug', action='store_true', default=False, dest='debug',
                   help='Build in debug mode')
    opt.add_option('--no-groups', action='store_true', default=False, dest='no_groups')
    opt.add_option('--sandboxed-build', action='store_true', default=False, dest='sandbox')


def configure(conf):
    """
    Configure the tools for the build by locating SDK prerequisites on the filesystem
    :param conf: the ConfigureContext
    :return: N/A
    """
    if not conf.options.debug:
        conf.env.append_value('DEFINES', 'RELEASE')
    else:
        Logs.pprint("CYAN", "Debug enabled")
    if conf.options.no_groups:
        conf.env.USE_GROUPS = False
    else:
        conf.env.USE_GROUPS = True
    conf.env.SANDBOX = conf.options.sandbox
    conf.env.VERBOSE = conf.options.verbose
    conf.env.TIMESTAMP = int(time.time())

    # If waf is in         ~/pebble-dev/PebbleSDK-X.XX/waf
    # Then this file is in ~/pebble-dev/PebbleSDK-X.XX/.waflib-xxxx/waflib/extras/
    # => we need to go up 3 directories to find the folder containing waf
    pebble_sdk = conf.root.find_dir(os.path.dirname(__file__)).parent.parent.parent
    if pebble_sdk is None:
        conf.fatal("Unable to find Pebble SDK!\n"
                   "Please make sure you are running waf directly from your SDK.")
    conf.env.PEBBLE_SDK_ROOT = pebble_sdk.abspath()

    # Set location of Pebble SDK common folder
    pebble_sdk_common = pebble_sdk.find_node('common')
    conf.env.PEBBLE_SDK_COMMON = pebble_sdk_common.abspath()

    if 'NODE_PATH' in os.environ:
        conf.env.NODE_PATH = conf.root.find_node(os.environ['NODE_PATH']).abspath()
        webpack_path = conf.root.find_node(conf.env.NODE_PATH).find_node('.bin').abspath()

        try:
            conf.find_program('webpack', path_list=[webpack_path])
        except conf.errors.ConfigurationError:
            pass  # Error will be caught after checking for enableMultiJS setting
    else:
        Logs.pprint('YELLOW', "WARNING: Unable to find $NODE_PATH variable required for SDK "
                              "build. Please verify this build was initiated with a recent "
                              "pebble-tool.")

    maybe_import_internal(conf.env)


def build(bld):
    """
    This method is invoked from the app or lib waftool with the `bld.load('pebble_sdk_common')`
    call and sets up additional task generators for the SDK.
    :param bld: the BuildContext object
    :return: N/A
    """

    # cached_env is set to a shallow copy of the current ConfigSet for this BuildContext
    bld.env = bld.all_envs['']
    bld.load('file_name_c_define')

    # Process message keys
    bld(features='message_keys')

    cached_env = bld.env
    for platform in bld.env.TARGET_PLATFORMS:
        # bld.env is set to a shallow copy of the ConfigSet labeled <platform>
        bld.env = bld.all_envs[platform]

        # Create a build group (set of TaskGens) for <platform>
        if bld.env.USE_GROUPS:
            bld.add_group(bld.env.PLATFORM_NAME)

        # Generate a linker script specific to the current platform
        build_node = bld.path.get_bld().find_or_declare(bld.env.BUILD_DIR)
        bld(features='subst',
            source=find_sdk_component(bld, bld.env, 'pebble_app.ld.template'),
            target=build_node.make_node('pebble_app.ld.auto'),
            **bld.env.PLATFORM)

        # Locate Rocky JS tooling script
        js_tooling_script = find_sdk_component(bld, bld.env, 'tools/generate_snapshot.js')
        bld.env.JS_TOOLING_SCRIPT = js_tooling_script if js_tooling_script else None

    # bld.env is set back to a shallow copy of the original ConfigSet that was set when this
    # `build` method was invoked
    bld.env = cached_env

    # Create a build group for bundling (should run after the build groups for each platform)
    if bld.env.USE_GROUPS:
        bld.add_group('bundle')


def _wrap_c_preproc_scan(task):
    """
    This function is a scanner function that wraps c_preproc.scan to fix up pebble.h dependencies.
    pebble.h is outside out the bld/src trees so therefore it's not considered a valid dependency
    and isn't scanned for further dependencies. Normally this would be fine but pebble.h includes
    an auto-generated resource id header which is really a dependency. We detect this include and
    add the resource id header file to the nodes being scanned by c_preproc.
    :param task: the task instance
    :return: N/A
    """
    (nodes, names) = c_preproc.scan(task)
    if 'pebble.h' in names:
        nodes.append(get_node_from_abspath(task.generator.bld, task.env.RESOURCE_ID_HEADER))
        nodes.append(get_node_from_abspath(task.generator.bld, task.env.MESSAGE_KEYS_HEADER))
    return nodes, names


@feature('c')
@before_method('process_source')
def setup_pebble_c(task_gen):
    """
    This method is called before all of the c aliases (objects, shlib, stlib, program, etc) and
    ensures that the SDK `include` path for the current platform, as well as the project root
    directory and the project src directory are included as header search paths (includes) for the
    build.
    :param task_gen: the task generator instance
    :return: N/A
    """
    platform = task_gen.env.PLATFORM_NAME
    append_to_attr(task_gen, 'includes',
                   [find_sdk_component(task_gen.bld, task_gen.env, 'include'),
                    '.', 'include', 'src'])
    append_to_attr(task_gen, 'includes', platform)
    for lib in task_gen.bld.env.LIB_JSON:
        if 'pebble' in lib:
            lib_include_node = task_gen.bld.path.find_node(lib['path']).find_node('include')
            append_to_attr(task_gen, 'includes',
                           [lib_include_node,
                            lib_include_node.find_node(str(lib['name'])).find_node(platform)])


@feature('c')
@after_method('process_source')
def fix_pebble_h_dependencies(task_gen):
    """
    This method is called before all of the c aliases (objects, shlib, stlib, program, etc) and
    ensures that the _wrap_c_preproc_scan method is run for all c tasks.
    :param task_gen: the task generator instance
    :return: N/A
    """
    for task in task_gen.tasks:
        if type(task) == c.c:
            # Swap out the bound member function for our own
            task.scan = types.MethodType(_wrap_c_preproc_scan, task, c.c)


@feature('pebble_cprogram')
@before_method('process_source')
def setup_pebble_cprogram(task_gen):
    """
    This method is called before all of the c aliases (objects, shlib, stlib, program, etc) and
    adds the appinfo.auto.c file to the source file list, adds the SDK pebble library to the lib
    path for the build, sets the linkflags for the build, and specifies the linker script to
    use during the linking step.
    :param task_gen: the task generator instance
    :return: None
    """
    build_node = task_gen.path.get_bld().make_node(task_gen.env.BUILD_DIR)
    platform = task_gen.env.PLATFORM_NAME
    if not hasattr(task_gen, 'bin_type') or getattr(task_gen, 'bin_type') != 'lib':
        append_to_attr(task_gen, 'source', build_node.make_node('appinfo.auto.c'))
        append_to_attr(task_gen, 'source', build_node.make_node('src/resource_ids.auto.c'))

        if task_gen.env.MESSAGE_KEYS:
            append_to_attr(task_gen,
                           'source',
                           get_node_from_abspath(task_gen.bld,
                                                 task_gen.env.MESSAGE_KEYS_DEFINITION))

    append_to_attr(task_gen, 'stlibpath',
                   find_sdk_component(task_gen.bld, task_gen.env, 'lib').abspath())
    append_to_attr(task_gen, 'stlib', 'pebble')

    for lib in task_gen.bld.env.LIB_JSON:
        # Skip binary check for non-Pebble libs
        if not 'pebble' in lib:
            continue

        binaries_path = task_gen.bld.path.find_node(lib['path']).find_node('binaries')
        if binaries_path:
            # Check for existence of platform folders inside binaries folder
            platform_binary_path = binaries_path.find_node(platform)
            if not platform_binary_path:
                task_gen.bld.fatal("Library {} is missing the {} platform folder in {}".
                                   format(lib['name'], platform, binaries_path))

            # Check for existence of binary for each platform
            if lib['name'].startswith('@'):
                scoped_name = lib['name'].rsplit('/', 1)
                lib_binary = (platform_binary_path.find_node(str(scoped_name[0])).
                              find_node("lib{}.a".format(scoped_name[1])))
            else:
                lib_binary = platform_binary_path.find_node("lib{}.a".format(lib['name']))

            if not lib_binary:
                task_gen.bld.fatal("Library {} is missing a binary for the {} platform".
                                   format(lib['name'], platform))

            # Link library binary (supports scoped names)
            if lib['name'].startswith('@'):
                append_to_attr(task_gen, 'stlibpath',
                               platform_binary_path.find_node(str(scoped_name[0])).abspath())
                append_to_attr(task_gen, 'stlib', scoped_name[1])
            else:
                append_to_attr(task_gen, 'stlibpath', platform_binary_path.abspath())
                append_to_attr(task_gen, 'stlib', lib['name'])

    append_to_attr(task_gen, 'linkflags',
                   ['-Wl,--build-id=sha1',
                    '-Wl,-Map,pebble-{}.map,--emit-relocs'.format(getattr(task_gen,
                                                                          'bin_type',
                                                                          'app'))])
    if not hasattr(task_gen, 'ldscript'):
        task_gen.ldscript = (
                build_node.find_or_declare('pebble_app.ld.auto').path_from(task_gen.path))


def _get_entry_point(ctx, js_type, waf_js_entry_point):
    """
    Returns the appropriate JS entry point, extracted from a project's package.json file,
    wscript or common SDK default
    :param ctx: the BuildContext
    :param js_type: type of JS build, pkjs or rockyjs
    :param waf_js_entry_point: the JS entry point specified by waftools
    :return: the JS entry point for the bundled JS file
    """
    fallback_entry_point = waf_js_entry_point
    if not fallback_entry_point:
        if js_type == 'pkjs':
            if ctx.path.find_node('src/pkjs/index.js'):
                fallback_entry_point = 'src/pkjs/index.js'
            else:
                fallback_entry_point = 'src/js/app.js'
        if js_type == 'rockyjs':
            fallback_entry_point = 'src/rocky/index.js'

    project_info = ctx.env.PROJECT_INFO

    if not project_info.get('main'):
        return fallback_entry_point
    if project_info['main'].get(js_type):
        return str(project_info['main'][js_type])
    return fallback_entry_point


@conf
def pbl_bundle(self, *k, **kw):
    """
    This method is bound to the build context and is called by specifying `bld.pbl_bundle`. We
    set the custom features `js` and `bundle` to run when this method is invoked.
    :param self: the BuildContext object
    :param k: none expected
    :param kw:
        binaries - a list containing dictionaries specifying the HW platform targeted by the
                   binary built, the app binary, and an optional worker binary
        js - the source JS files to be bundled
        js_entry_file - an optional parameter to specify the entry JS file when
                        enableMultiJS is set to 'true'
    :return: a task generator instance with keyword arguments specified
    """
    if kw.get('bin_type', 'app') == 'lib':
        kw['features'] = 'headers js package'
    else:
        if self.env.BUILD_TYPE == 'rocky':
            kw['js_entry_file'] = _get_entry_point(self, 'pkjs', kw.get('js_entry_file'))
        kw['features'] = 'js bundle'
    return self(*k, **kw)


@conf
def pbl_build(self, *k, **kw):
    """
    This method is bound to the build context and is called by specifying `bld.pbl_build()`. We
    set the custom features `c`, `cprogram` and `pebble_cprogram` to run when this method is
    invoked. This method is intended to someday replace `pbl_program` and `pbl_worker` so that
    all apps, workers, and libs will run through this method.
    :param self: the BuildContext object
    :param k: none expected
    :param kw:
        source - the source C files to be built and linked
        target - the destination binary file for the compiled source
    :return: a task generator instance with keyword arguments specified
    """
    valid_bin_types = ('app', 'worker', 'lib', 'rocky')
    bin_type = kw.get('bin_type', None)
    if bin_type not in valid_bin_types:
        self.fatal("The pbl_build method requires that a valid bin_type attribute be specified. "
                   "Valid options are {}".format(valid_bin_types))

    if bin_type == 'rocky':
        kw['features'] = 'c cprogram pebble_cprogram memory_usage'
    elif bin_type in ('app', 'worker'):
        kw['features'] = 'c cprogram pebble_cprogram memory_usage'
        kw[bin_type] = kw['target']
    elif bin_type == 'lib':
        kw['features'] = 'c cstlib memory_usage'
        path, name = kw['target'].rsplit('/', 1)
        kw['lib'] = self.path.find_or_declare(path).make_node("lib{}.a".format(name))

    # Pass values needed for memory usage report
    if bin_type != 'worker':
        kw['resources'] = (
            self.env.PROJECT_RESBALL if bin_type == 'lib' else
            self.path.find_or_declare(self.env.BUILD_DIR).make_node('app_resources.pbpack'))
    return self(*k, **kw)


@conf
def pbl_js_build(self, *k, **kw):
    """
    This method is bound to the build context and is called by specifying `bld.pbl_cross_compile()`.
    When this method is invoked, we set the custom feature `rockyjs` to run, which handles
    processing of JS files in preparation for Rocky.js bytecode compilation (this actually
    happens during resource generation)
    :param self: the BuildContext object
    :param k: none expected
    :param kw:
        source - the source JS files that will eventually be compiled into bytecode
        target - the destination JS file that will be specified as the source file for the
        bytecode compilation process
    :return: a task generator instance with keyword arguments specified
    """
    kw['js_entry_file'] = _get_entry_point(self, 'rockyjs', kw.get('js_entry_file'))
    kw['features'] = 'rockyjs'
    return self(*k, **kw)
