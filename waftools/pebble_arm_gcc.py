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
import re
import waflib
from waflib import Utils
from waflib.Configure import conf

def find_clang_path(conf):
  """ Find the first clang on our path with a version greater than 3.2"""

  out = conf.cmd_and_log('which -a clang')
  paths = out.splitlines()
  for path in paths:
    # Make sure clang is at least version 3.3
    out = conf.cmd_and_log('%s --version' % path)
    r = re.findall(r'clang version (\d+)\.(\d+)', out)
    if len(r):
      version_major = int(r[0][0])
      version_minor = int(r[0][1])
      if version_major > 3 or (version_major == 3 and version_minor >= 3):
        return path

  conf.fatal('No version of clang 3.3+ found on your path!')

def find_toolchain_path(conf):
  possible_paths = ['~/arm-cs-tools/arm-none-eabi',
                    '/usr/local/Cellar/arm-none-eabi-gcc/arm/arm-none-eabi']
  for p in possible_paths:
      if os.path.isdir(p):
          return p

  conf.fatal('could not find arm-none-eabi folder')

def find_sysroot_path(conf):
  """ The sysroot is a directory struct that looks like /usr/bin/ that includes custom
      headers and libraries for a target. We want to use the headers from our mentor
      toolchain, but they don't have a structure like /usr/bin. Therefore, if this is
      first time configuring on a system, create the directory structure with the
      appropriate symlinks so things work out.

      Note that this is a bit of a hack. Ideally our toolchain setup script will produce
      the directory structure that we expect, but I'm not going to muck with the toolchain
      too much until I get everything working end to end as opposed to having to rebuild
      our toolchain all the time. """

  toolchain_path = find_toolchain_path(conf)

  sysroot_path = os.path.join(toolchain_path, 'sysroot')
  if not os.path.isdir(sysroot_path):
    waflib.Logs.pprint('CYAN', 'Sysroot dir not found at %s, creating...', sysroot_path)
    os.makedirs(os.path.join(sysroot_path, 'usr/local/'))

    os.symlink(os.path.join(toolchain_path, 'include/'),
               os.path.join(sysroot_path, 'usr/local/include'))

  return sysroot_path

@conf
def using_clang_compiler(ctx):
    compiler_name = ctx.env.CC
    if isinstance(ctx.env.CC, list):
        compiler_name = ctx.env.CC[0]

    if 'CCC_CC' in os.environ:
        compiler_name = os.environ['CCC_CC']

    return 'clang' in compiler_name


def options(opt):
    opt.add_option('--relax_toolchain_restrictions', action='store_true',
                   help='Allow us to compile with a non-standard toolchain')
    opt.add_option('--use_clang', action='store_true',
                   help='(EXPERIMENTAL) Uses clang instead of gcc as our compiler')
    opt.add_option('--use_env_cc', action='store_true',
                   help='Use whatever CC is in the environment as our compiler')
    opt.add_option('--beta', action='store_true',
                   help='Build in beta mode '
                        '(--beta and --release are mutually exclusive)')
    opt.add_option('--release', action='store_true',
                   help='Build in release mode'
                        ' (--beta and --release are mutually exclusive)')
    opt.add_option('--fat_firmware', action='store_true',
                   help='build in GDB mode WITH logs; requires 1M of onboard flash')
    opt.add_option('--gdb', action='store_true',
                   help='build in GDB mode (no optimization, no logs)')
    opt.add_option('--lto', action='store_true', help='Enable link-time optimization')
    opt.add_option('--no-lto', action='store_true', help='Disable link-time optimization')
    opt.add_option('--save_temps', action='store_true',
                   help='Save *.i and *.s files during compilation')
    opt.add_option('--no_debug', action='store_true',
                   help='Remove -g debug information. See --save_temps')

def configure(conf):
    CROSS_COMPILE_PREFIX = 'arm-none-eabi-'

    conf.env.AS = CROSS_COMPILE_PREFIX + 'gcc'
    conf.env.AR = CROSS_COMPILE_PREFIX + 'gcc-ar'
    if conf.options.use_env_cc:
      pass # Don't touch conf.env.CC
    elif conf.options.use_clang:
      conf.env.CC = find_clang_path(conf)
    else:
      conf.env.CC = CROSS_COMPILE_PREFIX + 'gcc'

    conf.env.LINK_CC = conf.env.CC

    conf.load('gcc')

    conf.env.append_value('CFLAGS', [ '-std=c11', ])

    c_warnings = [ '-Wall',
                   '-Wextra',
                   '-Werror',
                   '-Wpointer-arith',
                   '-Wno-unused-parameter',
                   '-Wno-missing-field-initializers',
                   '-Wno-error=unused-function',
                   '-Wno-error=unused-variable',
                   '-Wno-error=unused-parameter' ]

    if conf.using_clang_compiler():
      sysroot_path = find_sysroot_path(conf)

      # Disable clang warnings from now... they don't quite match
      c_warnings = []

      conf.env.append_value('CFLAGS', [ '-target', 'arm-none-eabi' ])
      conf.env.append_value('CFLAGS', [ '--sysroot', sysroot_path ])

      # Clang doesn't enable short-enums by default since
      # arm-none-eabi is an unsupported target
      conf.env.append_value('CFLAGS', '-fshort-enums')

      arm_toolchain_path = find_toolchain_path(conf)
      conf.env.append_value('CFLAGS', [ '-B' + arm_toolchain_path ])

      conf.env.append_value('LINKFLAGS', [ '-target', 'arm-none-eabi' ])
      conf.env.append_value('LINKFLAGS', [ '--sysroot', sysroot_path ])

    else:
      # These warnings only exist in GCC
      c_warnings.append('-Wno-error=unused-but-set-variable')
      c_warnings.append('-Wno-packed-bitfield-compat')

      if not ('4', '8') <= conf.env.CC_VERSION <= ('4', '9', '3'):
        # Verify the toolchain we're using is allowed. This is to prevent us from accidentally
        # building and releasing firmwares that are built in ways we haven't tested.

        if not conf.options.relax_toolchain_restrictions:
            TOOLCHAIN_ERROR_MSG = \
"""=== INVALID TOOLCHAIN ===
Either upgrade your toolchain using the process listed here:
    https://pebbletechnology.atlassian.net/wiki/display/DEV/Firmware+Toolchain
Or re-configure with the --relax_toolchain_restrictions option. """

            conf.fatal('Invalid toolchain detected!\n' + \
                       repr(conf.env.CC_VERSION) + '\n' + \
                       TOOLCHAIN_ERROR_MSG)

    conf.env.CFLAGS.append('-I' + conf.path.abspath() + '/src/fw/util/time')

    conf.env.append_value('CFLAGS', c_warnings)

    conf.add_platform_defines(conf.env)

    conf.env.ASFLAGS = [ '-xassembler-with-cpp', '-c' ]
    conf.env.AS_TGT_F = '-o'

    conf.env.append_value('LINKFLAGS', [ '-Wl,--warn-common' ])

    args = [ '-fvar-tracking-assignments',  # Track variable locations better
             '-mthumb',
             '-ffreestanding',
             '-ffunction-sections',
             '-fbuiltin',
             '-fno-builtin-itoa' ]

    if not conf.options.no_debug:
        args += [ '-g3',  # Extra debugging info, including macro definitions
                  '-gdwarf-4' ] # More detailed debug info

    if conf.options.save_temps:
        args += [ '-save-temps=obj' ]

    if conf.options.lto:
        args += [ '-flto' ]
        if not using_clang_compiler(conf):
            # None of these options are supported by clang
            args += [ '-flto-partition=balanced',
                      '--param','lto-partitions=128', # Can be trimmed down later
                      '-fuse-linker-plugin',
                      '-fno-if-conversion',
                      '-fno-caller-saves',
                      '-fira-region=mixed',
                      '-finline-functions',
                      '-fconserve-stack',
                      '--param','inline-unit-growth=1',
                      '--param','max-inline-insns-auto=1',
                      '--param','max-cse-path-length=1000',
                      '--param','max-grow-copy-bb-insns=1',
                      '-fno-hoist-adjacent-loads',
                      '-fno-optimize-sibling-calls',
                      '-fno-schedule-insns2' ]

    cpu_fpu = None
    if conf.env.MICRO_FAMILY == "STM32F2":
        args += [ '-mcpu=cortex-m3' ]
    elif conf.env.MICRO_FAMILY == "STM32F4":
        args += [ '-mcpu=cortex-m4']
        cpu_fpu = "fpv4-sp-d16"
    elif conf.env.MICRO_FAMILY == "STM32F7":
        args += [ '-mcpu=cortex-m7']
        cpu_fpu = "fpv5-d16"
    # QEMU does not have FPU
    if conf.env.QEMU:
        cpu_fpu = None

    if cpu_fpu:
      args += [ "-mfloat-abi=softfp",
                "-mfpu="+cpu_fpu ]
    else:
      # Not using float-abi=softfp means no FPU instructions.
      # It also defines __SOFTFP__=1
      # Yes that define name is super misleading, but what can you do.
      pass

    conf.env.append_value('CFLAGS', args)
    conf.env.append_value('ASFLAGS', args)
    conf.env.append_value('LINKFLAGS', args)

    conf.env.SHLIB_MARKER = None
    conf.env.STLIB_MARKER = None

    # Set whether or not we show the "Your Pebble just reset..." alert
    if conf.options.release and conf.options.beta:
      raise RuntimeError("--beta and --release are mutually exclusive and cannot be used together")
    if not conf.options.release:
      conf.env.append_value('DEFINES', [ 'SHOW_PEBBLE_JUST_RESET_ALERT' ])
      conf.env.append_value('DEFINES', [ 'SHOW_BAD_BT_STATE_ALERT' ])
      if not conf.is_bigboard():
        conf.env.append_value('DEFINES', [ 'SHOW_ACTIVITY_DEMO' ])

    # Set optimization level
    if conf.options.beta:
        optimize_flags = '-Os'
        print "Beta mode"
    elif conf.options.release:
        optimize_flags = '-Os'
        print "Release mode"
    elif conf.options.fat_firmware:
        optimize_flags = '-O0'
        conf.env.IS_FAT_FIRMWARE = True
        print 'Building Fat Firmware (no optimizations, logging enabled)'
    elif conf.options.gdb:
        optimize_flags = '-Og'
        print "GDB mode"
    else:
        optimize_flags = '-Os'
        print 'Debug Mode'

    conf.env.append_value('CFLAGS', optimize_flags)
    conf.env.append_value('LINKFLAGS', optimize_flags)
