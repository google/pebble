import json
import os
import subprocess
import sys
import pexpect
import zipfile
import datetime
import time

import waflib
from waflib import Node, Logs
from waflib.Build import BuildContext


waf_dir = sys.path[0]
sys.path.append(os.path.join(waf_dir, 'tools'))
sys.path.append(os.path.join(waf_dir, 'tools/log_hashing'))
sys.path.append(os.path.join(waf_dir, 'sdk/tools/'))
sys.path.append(os.path.join(waf_dir, 'waftools'))

import waftools.asm
import waftools.gitinfo
import waftools.ldscript
import waftools.openocd
import waftools.xcode_pebble

LOGHASH_OUT_PATH = 'src/fw/loghash_dict.json'

def truncate(msg):
    if msg is None:
        return msg

    # Don't truncate exceptions thrown by waf itself
    if "Traceback " in msg:
        return msg

    truncate_length = 600
    if len(msg) > truncate_length:
        msg = msg[:truncate_length-4] + '...\n' + waflib.Logs.colors.NORMAL
    return msg


def get_comma_separated_args(option, opt, value, parser):
    setattr(parser.values, option.dest, value.split(','))


old_display = waflib.Task.TaskBase.display
def wrap_display(self):
    return truncate(old_display(self))
waflib.Task.TaskBase.display = wrap_display

old_format_error = waflib.Task.TaskBase.format_error
def wrap_format_error(self):
    return truncate(old_format_error(self))
waflib.Task.TaskBase.format_error = wrap_format_error


def run_arm_gdb(ctx, elf_node, cmd_str="", target_server_port=3333):
    from tools.gdb_driver import find_gdb_path
    arm_none_eabi_path = find_gdb_path()
    if arm_none_eabi_path is None:
        ctx.fatal("pebble-gdb not found!")
    os.system('{} {} {} --ex="target remote :{}"'.format(
                arm_none_eabi_path, elf_node.path_from(ctx.path),
                cmd_str, target_server_port)
              )


def options(opt):
    opt.load('pebble_arm_gcc', tooldir='waftools')
    opt.load('show_configure', tooldir='waftools')
    opt.recurse('applib-targets')
    opt.recurse('tests')
    opt.recurse('src/bluetooth-fw')
    opt.recurse('src/fw')
    opt.recurse('src/idl')
    opt.recurse('sdk')
    opt.add_option('--board', action='store',
                   choices=[ 'bb2',
                             'ev2_4',
                             'v1_5',
                             'v2_0',
                             'snowy_bb2',  # alias for snowy_dvt, but with #define IS_BIGBOARD
                             'snowy_evt2',
                             'snowy_dvt',
                             'snowy_s3',
                             'spalding_bb2',  # snowy_bb2 with s4 display
                             'spalding_evt',
                             'spalding',
                             'silk_evt',
                             'silk_bb',
                             'silk',
                             'silk_bb2',
                             'cutts_bb',
                             'robert_bb',
                             'robert_bb2',
                             'robert_evt',
                             'robert_es'],
                   help='Which board we are targeting '
                        'bb2, snowy_dvt, spalding, silk...')
    opt.add_option('--jtag', action='store', default=None, dest='jtag',  # default is bb2 (below)
                   choices=waftools.openocd.JTAG_OPTIONS.keys(),
                   help='Which JTAG programmer we are using '
                        '(bb2 (default), olimex, ev2, etc)')
    opt.add_option('--internal_sdk_build', action='store_true',
                   help='Build the internal version of the SDK')
    opt.add_option('--future_ux', action='store_true',
                   help='Build future UX features and APIs. Implies --internal_sdk_build.')
    opt.add_option('--nosleep', action='store_true',
                   help='Disable sleep and stop mode (to use JTAG+GDB)')
    opt.add_option('--nostop', action='store_true',
                   help='Disable stop mode (to use JTAG+GDB)')
    opt.add_option('--lowpowerdebug', action='store_true',
                   help='Lowpowerdebug can be toggled from the CLI but is off by default. This just turns it on by default')
    opt.add_option('--nowatch', action='store_true',
                   help='Disable the watchface idle timeout')
    opt.add_option('--nowatchdog', action='store_true',
                   help='Disable automatic reboots when watchdog fires')
    opt.add_option('--test_apps', action='store_true',
                   help='Enables test apps (off by default)')
    opt.add_option('--test_apps_list', action='callback', type='string', callback=get_comma_separated_args,
                   help='Specify AppInstallId\'s of the test apps to be compiled with the firmware')
    opt.add_option('--performance_tests', action='store_true',
                   help='Enables instrumentation + apps for performance testing (off by default)')
    opt.add_option('--verbose_logs', action='store_true',
                   help='Enables verbose logs (off by default)')
    opt.add_option('--ui_debug', action='store_true',
                   help='Enable window dump & layer nudge CLI cmd (off by default)')
    opt.add_option('--qemu', action='store_true',
                   help='Build an image for qemu instead of a real board.')
    opt.add_option('--nojs', action='store_true', help='Removes js support from the current build.')
    opt.add_option('--sdkshell', action='store_true',
                   help='Use the sdk shell instead of the normal shell')
    opt.add_option('--nolog', action='store_true',
                   help='Disable PBL_LOG macros to save space')
    opt.add_option('--nohash', action='store_true',
                   help='Disable log hashing and make the logs human readable')

    opt.add_option('--lang',
                   action='store',
                   default='en_US',
                   help='Which language to package (isocode)')

    opt.add_option('--compile_commands', action='store_true', help='Create a clang compile_commands.json')
    opt.add_option('--file', action='store', help='Specify a file to use with the flash_fw command')
    opt.add_option('--tty',
        help='Selects a tty to use for serial imaging. Must be specified for all image commands')
    opt.add_option('--baudrate', action='store', type=int, help='Optional: specifies the baudrate to run the targetted uart at')
    opt.add_option('--onlysdk', action='store_true', help="only build the sdk")
    opt.add_option('--qemu_host', default='localhost:12345',
        help='host:port for the emulator console connection')
    opt.add_option('--force-fit-tintin', action='store_true',
                   help='Force fit for Tintin')
    opt.add_option('--no-link', action='store_true',
                   help='Do not link the final firmware binary. This is used for static analysis')
    opt.add_option('--noprompt', action='store_true',
                   help='Disable the serial console to save space')
    opt.add_option('--build_test_apps', action='store_true',
                   help='Turns on building of test apps')
    opt.add_option('--bb_large_spi', action='store_true',
                   help='Sets a flag to use all 8MB of BigBoard flash')
    opt.add_option('--profiler', action='store_true', help='Enable the profiler.')
    opt.add_option('--profile_interrupts', action='store_true',
                   help='Enable profiling of all interrupts.')
    opt.add_option('--voice_debug', action='store_true',
                   help='Enable all voice logging.')
    opt.add_option('--voice_codec_tests', action='store_true',
                   help='Enable voice codec tests. Enables the profiler')
    opt.add_option('--battery_debug', action='store_true',
                   help='Set the PMIC\'s max charging voltage to 4.3V.')
    opt.add_option('--no_sandbox', action='store_true',
                   help='Disable the MPU for 3rd party apps.')
    opt.add_option('--malloc_instrumentation', action='store_true',
                   help='Enables malloc instrumentation')
    opt.add_option('--infinite_backlight', action='store_true',
                   help='Makes the backlight never time-out.')
    opt.add_option('--mfg', action='store_true', help='Enable specific MFG-only options in the PRF build')
    opt.add_option('--no-pulse-everywhere',
                   action='store_true',
                   help='Disables PULSE everywhere, uses legacy logs and prompt')
    opt.add_option('--bootloader-test', action='store', default='none',
                   choices=['none', 'stage1', 'stage2'],
                   help='Build bootloader test (stage1 or stage2). Implies --mfg.')
    opt.add_option('--reboot_on_bt_crash', action='store_true', help='Forces a BT '
                   'chip crash to immediately force a system reboot instead of just cycling airplane mode. '
                   'This makes it easier for us to actually get crash info')


def handle_configure_options(conf):
    if conf.options.noprompt:
        conf.env.append_value('DEFINES', 'DISABLE_PROMPT')
        conf.env.DISABLE_PROMPT = True

    if conf.options.beta or conf.options.release:
        conf.env.append_value('DEFINES', 'RELEASE')

    if conf.options.malloc_instrumentation:
        conf.env.append_value('DEFINES', 'MALLOC_INSTRUMENTATION')
        print "Enabling malloc instrumentation"

    if conf.options.qemu:
        conf.env.append_value('DEFINES', 'TARGET_QEMU')

    if conf.options.test_apps_list:
        conf.options.test_apps = True
        conf.env.test_apps_list = conf.options.test_apps_list
        print "Enabling test apps: " + str(conf.options.test_apps_list)

    if conf.options.build_test_apps or conf.options.test_apps:
        conf.env.BUILD_TEST_APPS = True

    if conf.options.performance_tests:
        conf.env.PERFORMANCE_TESTS = True

    if conf.options.voice_debug:
        conf.env.VOICE_DEBUG = True

    if conf.options.voice_codec_tests:
        conf.env.VOICE_CODEC_TESTS = True
        conf.env.append_value('DEFINES', 'VOICE_CODEC_TESTS')
        conf.options.profiler = True

    if conf.env.MICRO_FAMILY == 'STM32F4':
        if conf.options.lowpowerdebug and not conf.options.nosleep:
            Logs.warn('On snowy --lowpowerdebug can only be used with --nosleep. Forcing --nosleep on!\n'
                      'See PBL-10174.')
            conf.env.append_value('DEFINES', 'PBL_NOSLEEP')

    if 'bb' in conf.options.board:
        conf.env.append_value('DEFINES', 'IS_BIGBOARD')

    if conf.options.nosleep:
        conf.env.append_value('DEFINES', 'PBL_NOSLEEP')
        print "Sleep/stop mode disabled"

    if conf.options.nostop:
        conf.env.append_value('DEFINES', 'PBL_NOSTOP')
        print "Stop mode disabled"

    if conf.options.lowpowerdebug:
        conf.env.append_value('DEFINES', 'LOW_POWER_DEBUG')
        print "Sleep and Stop mode debugging enabled"

    if conf.options.nowatch:
        conf.env.append_value('DEFINES', 'NO_WATCH_TIMEOUT')
        print "Watch watchdog disabled"

    if conf.options.nowatchdog:
        conf.env.append_value('DEFINES', 'NO_WATCHDOG')
        conf.env.NO_WATCHDOG = True
        print "Watchdog reboot disabled"

    if conf.options.reboot_on_bt_crash:
        conf.env.append_value('DEFINES', 'REBOOT_ON_BT_CRASH=1')
        print "BT now crash will trigger an MCU reboot"

    if conf.options.test_apps:
        conf.env.append_value('DEFINES', 'ENABLE_TEST_APPS')
        print "Im in ur firmware, bloatin ur binz! (Test apps enabled)"

    if conf.options.performance_tests:
        conf.env.append_value('DEFINES', 'PERFORMANCE_TESTS')
        conf.options.profiler = True
        print "Instrumentation and apps for performance measurement enabled (enables profiler)"

    if conf.options.verbose_logs:
        conf.env.append_value('DEFINES', 'VERBOSE_LOGGING')
        print "Verbose logging enabled"

    if conf.options.ui_debug:
        conf.env.append_value('DEFINES', 'UI_DEBUG')

    if conf.options.no_sandbox or conf.options.qemu:
        print "Sandbox disabled"
    else:
        conf.env.append_value('DEFINES', 'APP_SANDBOX')

    if conf.options.bb_large_spi:
        conf.env.append_value('DEFINES', 'LARGE_SPI_FLASH')
        print "Enabling 8MB BigBoard flash"

    if not conf.options.nolog:
        conf.env.append_value('DEFINES', 'PBL_LOG_ENABLED')
        if not conf.options.nohash:
            conf.env.append_value('DEFINES', 'PBL_LOGS_HASHED')

    if conf.options.profile_interrupts:
        conf.env.append_value('DEFINES', 'PROFILE_INTERRUPTS')
        if not conf.options.profiler:
            # Can't profile interrupts without the profiler enabled
            print "Enabling profiler"
            conf.options.profiler = True

    if conf.options.profiler:
        conf.env.append_value('DEFINES', 'PROFILER')
        if not conf.options.nostop:
            print "Enable --nostop for accurate profiling."
            conf.env.append_value('DEFINES', 'PBL_NOSTOP')

    if conf.options.voice_debug:
        conf.env.append_value('DEFINES', 'VOICE_DEBUG')

    if conf.options.battery_debug:
        conf.env.append_value('DEFINES', 'BATTERY_DEBUG')
        print "Enabling higher battery charge voltage."

    if conf.options.future_ux and not conf.is_tintin():
        print "Future UX features enabled."
        conf.env.FUTURE_UX = True

    conf.env.INTERNAL_SDK_BUILD = bool(conf.options.internal_sdk_build)
    if conf.env.INTERNAL_SDK_BUILD:
        print "Internal SDK enabled"

    if conf.options.force_fit_tintin:
        conf.env.append_value('DEFINES', 'TINTIN_FORCE_FIT')
        print "Functionality is secondary to usability"

    if (conf.is_snowy_compatible() and not conf.options.no_lto) or conf.options.lto:
        conf.options.lto = True
        print "Turning on LTO."

    if conf.options.no_link:
        conf.env.NO_LINK = True
        print "Not linking firmware"

    if conf.options.infinite_backlight and 'bb' in conf.options.board:
        conf.env.append_value('DEFINES', 'INFINITE_BACKLIGHT')
        print "Enabling infinite backlight."

    if conf.options.bootloader_test in ['stage1', 'stage2']:
        print "Forcing MFG on for bootloader test build."
        conf.options.mfg = True

    if conf.options.bootloader_test == 'stage1':
        conf.env.append_value('DEFINES', 'BOOTLOADER_TEST_STAGE1=1')
        conf.env.append_value('DEFINES', 'BOOTLOADER_TEST_STAGE2=0')
    elif conf.options.bootloader_test == 'stage2':
        conf.env.append_value('DEFINES', 'BOOTLOADER_TEST_STAGE1=0')
        conf.env.append_value('DEFINES', 'BOOTLOADER_TEST_STAGE2=1')
    else:
        conf.env.append_value('DEFINES', 'BOOTLOADER_TEST_STAGE1=0')
        conf.env.append_value('DEFINES', 'BOOTLOADER_TEST_STAGE2=0')

    if not conf.options.mfg and not conf.options.no_pulse_everywhere:
        conf.env.append_value('DEFINES', 'PULSE_EVERYWHERE=1')

def _create_cm0_env(conf):
    prev_env = conf.env
    prev_variant = conf.variant

    # Create a new Cortex M0 environment that's used to build for the DA14681:
    conf.setenv('cortex-m0')

    # Copy the defines fron the stock env into our m0 env
    conf.env.append_unique('DEFINES', prev_env.DEFINES)

    Logs.pprint('CYAN', 'Configuring ARM cortex-m0 environment')

    conf.env.append_unique('DEFINES', 'ARCH_NO_NATIVE_LONG_DIVIDE')

    CPU_FLAGS = ['-mcpu=cortex-m0', '-mthumb']
    OPT_FLAGS = [
        '-fvar-tracking-assignments',  # Track variable locations better
        '-fmessage-length=0', '-fsigned-char',
        '-fbuiltin',
        '-fno-builtin-itoa',
        '-ffreestanding',
        '-Os',
    ]
    if not conf.options.no_debug:
        OPT_FLAGS += [
            '-g3',
            '-gdwarf-4',  # More detailed debug info
        ]

    C_FLAGS = ['-std=c11', '-ffunction-sections',
               '-Wall', '-Wextra', '-Werror', '-Wpointer-arith',
               '-Wno-unused-parameter', '-Wno-missing-field-initializers',
               '-Wno-error=unused-parameter',
               '-Wno-packed-bitfield-compat']

    conf.find_program('arm-none-eabi-gcc', var='CC', mandatory=True)
    conf.env.AS = conf.env.CC
    for tool in ['ar', 'objcopy']:
        conf.find_program('arm-none-eabi-' + tool, var=tool.upper(),
                          mandatory=True)

    conf.env.append_unique('CFLAGS', CPU_FLAGS + OPT_FLAGS + C_FLAGS)

    ASFLAGS = ['-x', 'assembler-with-cpp']
    conf.env.append_unique('ASFLAGS', ASFLAGS + CPU_FLAGS + OPT_FLAGS)

    conf.env.append_unique('LINKFLAGS',
                           ['-Wl,--cref',
                            '-Wl,--gc-sections',
                            '-nostdlib',
                            ] + CPU_FLAGS + OPT_FLAGS)

    conf.load('gcc gas objcopy ldscript')
    conf.load('file_name_c_define')

    conf.variant = prev_variant
    conf.env = prev_env


def configure(conf):
    if not conf.options.board:
        conf.fatal('No board selected! '
                   'You must pass a --board argument when configuring.')

    # Has to be 'waftools.gettext' as unadorned 'gettext' will find the gettext
    # module in the standard library.
    conf.load('waftools.gettext')

    conf.recurse('platform')

    conf.env.QEMU = conf.options.qemu
    conf.env.NOJS = conf.options.nojs

    # The BT controller is the only thing different between robert_es and robert_evt, so just
    # pretend robert_es is robert_evt. We'll be removing robert_es fairly soon anyways.
    bt_board = None
    if conf.options.board == 'robert_es':
        bt_board = 'robert_es'
        conf.options.board = 'robert_evt'

    if conf.options.jtag:
        conf.env.JTAG = conf.options.jtag
    elif conf.options.board in ('snowy_bb2', 'spalding_bb2'):
        conf.env.JTAG = 'jtag_ftdi'
    elif conf.options.board in ('cutts_bb', 'robert_bb', 'robert_bb2', 'robert_evt',
                                'silk_evt', 'silk_bb', 'silk_bb2', 'silk'):
        conf.env.JTAG = 'swd_ftdi'
    else:
        # default to bb2
        conf.env.JTAG = 'bb2'

    # Cutts and Robert access flash through the ITCM bus (except in QEMU)
    if (conf.is_cutts() or conf.is_robert()) and not conf.env.QEMU:
        conf.env.FLASH_ITCM = True
    else:
        conf.env.FLASH_ITCM = False

    # Set platform used for building the SDK
    if conf.is_tintin():
        conf.env.PLATFORM_NAME = 'aplite'
        conf.env.MIN_SDK_VERSION = 2
    elif conf.is_spalding():
        conf.env.PLATFORM_NAME = 'chalk'
        conf.env.MIN_SDK_VERSION = 3
    elif conf.is_snowy_compatible():
        conf.env.PLATFORM_NAME = 'basalt'
        conf.env.MIN_SDK_VERSION = 2
    elif conf.is_silk():
        conf.env.PLATFORM_NAME = 'diorite'
        conf.env.MIN_SDK_VERSION = 2
    elif conf.is_cutts() or conf.is_robert():
        conf.env.PLATFORM_NAME = 'emery'
        conf.env.MIN_SDK_VERSION = 3
    else:
        conf.fatal('No platform specified for {}!'.format(conf.options.board))

    # Save this for later
    conf.env.BOARD = conf.options.board

    if conf.is_tintin():
        conf.env.MICRO_FAMILY = 'STM32F2'
    elif conf.is_snowy_compatible() or conf.is_silk():
        conf.env.MICRO_FAMILY = 'STM32F4'
    elif conf.is_cutts() or conf.is_robert():
        conf.env.MICRO_FAMILY = 'STM32F7'
    else:
        conf.fatal('No micro family specified for {}!'.format(conf.options.board))

    if conf.options.mfg:
        # Note that for the most part PRF and MFG firmwares are the same, so for MFG PRF builds
        # both MANUFACTURING_FW and RECOVERY_FW will be defined.
        conf.env.IS_MFG = True
        conf.env.append_value('DEFINES', ['MANUFACTURING_FW'])

    conf.find_program('node nodejs', var='NODE',
                      errmsg="Unable to locate the Node command. "
                             "Please check your Node installation and try again.")

    conf.recurse('src/idl')
    conf.recurse('src/fw')
    conf.recurse('sdk')

    conf.recurse('bin/boot')
    waftools.openocd.write_cfg(conf)

    # Save a baseline environment that we'll use for unit tests
    # Detach so operations against conf.env don't affect unit_test_env
    unit_test_env = conf.env.derive()
    unit_test_env.detach()

    # Save a baseline environment that we'll use for ARM environments
    base_env = conf.env

    handle_configure_options(conf)


    # robert_es is the exact same as robert_evt, except for the BT chip, so gets converted to
    # robert_evt above, but we need to handle it as robert_es here.
    if bt_board is None:
        bt_board = conf.get_board()
    # Select BT controller based on configuration:
    if conf.env.QEMU:
        conf.env.bt_controller = 'qemu'
        conf.env.append_value('DEFINES', ['BT_CONTROLLER_QEMU'])
    elif conf.is_tintin() or conf.is_snowy() or conf.is_spalding():
        conf.env.bt_controller = 'cc2564x'
        conf.env.append_value('DEFINES', ['BT_CONTROLLER_CC2564X'])
    elif bt_board in ('silk_bb2', 'silk', 'robert_bb2', 'robert_evt'):
        conf.env.bt_controller = 'da14681-01'
        conf.env.append_value('DEFINES', ['BT_CONTROLLER_DA14681'])
    else:
        conf.env.bt_controller = 'da14681-00'
        conf.env.append_value('DEFINES', ['BT_CONTROLLER_DA14681'])

    _create_cm0_env(conf)

    conf.recurse('src/bluetooth-fw')

    Logs.pprint('CYAN', 'Configuring arm_firmware environment')
    conf.setenv('', base_env)
    conf.load('pebble_arm_gcc', tooldir='waftools')

    conf.setenv('arm_prf_mode', env=conf.env)
    conf.env.append_value('DEFINES', ['RECOVERY_FW'])

    Logs.pprint('CYAN', 'Configuring unit test environment')
    conf.setenv('local', unit_test_env)

    if sys.platform.startswith('linux'):
        libclang_path = subprocess.check_output(['llvm-config', '--libdir']).strip()
        conf.env.append_value('INCLUDES', [os.path.join(libclang_path, 'clang/3.2/include/'),])

    # The waf clang tool likes to use llvm-ar as it's ar tool, but that doesn't work on our build
    # servers. Fall back to boring old ar. This will populate the 'AR' env variable so future
    # searches for what value to put into env['AR'] will find this one.
    conf.find_program('ar')

    conf.load('clang')
    conf.load('pebble_test', tooldir='waftools')

    conf.env.CLAR_DIR = conf.path.make_node('tools/clar/').abspath()
    conf.env.CFLAGS = [ '-std=c11',

                        '-Wall',
                        '-Werror',
                        '-Wno-error=unused-variable',
                        '-Wno-error=unused-function',
                        '-Wno-error=missing-braces',

                        '-g3',
                        '-gdwarf-4',
                        '-O0',
                        '-fdata-sections',
                        '-ffunction-sections' ]

    conf.env.append_value('DEFINES', 'CLAR_FIXTURE_PATH="' +
                                     conf.path.make_node('tests/fixtures/').abspath() + '"')

    conf.env.append_value('DEFINES', 'PBL_LOG_ENABLED')

    if conf.options.compile_commands:
        conf.load('clang_compilation_database', tooldir='waftools')

        if not os.path.lexists('compile_commands.json'):
            filename = 'compile_commands.json'
            source = conf.path.get_bld().make_node(filename)
            os.symlink(source.path_from(conf.path), filename)

    prev_env = conf.env
    Logs.pprint('CYAN', 'Configuring 32 bit host environment')
    # Copy 'local' to serve as the basis for '32bit':
    env_32bit = conf.env.derive().detach()
    env_32bit.append_value('CFLAGS', '-m32')
    env_32bit.append_value('LINKFLAGS', '-m32')
    env_32bit.LINK_CC = 'gcc'
    conf.all_envs['32bit'] = env_32bit
    conf.set_env(prev_env)

    # Note: this will modify the 'local' conf when targeting emscripten:
    conf.recurse('applib-targets')

    Logs.pprint('CYAN', 'Configuring stored apps environment')
    conf.setenv('stored_apps', base_env)
    conf.recurse('stored_apps')

    # Confirm that requirements-*.txt and requirements-osx-brew.txt have been satisfied.
    import tool_check
    tool_check.tool_check()

    # Warn user not to use Cutts BB build with a Robert screen
    if conf.options.board == 'cutts_bb':
        Logs.warn('NOTE: Do not use this build with a C2/Robert display '
                  '(6V6 rail will damage the display)')


def _run_remote_suite(ctx, suite):
    # PEBBLESDK_TEST_ROOT must be defined in order to initiate integration tests
    try:
        pebblesdk_test_root = os.environ['PEBBLESDK_TEST_ROOT']
    except KeyError:
        waflib.Logs.pprint('RED', 'Error: environment variable $PEBBLESDK_TEST_ROOT must be defined')
        return

    # Check if firmware has been built
    # Assume we're looking for a "normal" PBZ, as recovery PBZs aren't supported by integration tests
    fw_bin_path = ctx.get_tintin_fw_node().abspath()
    fw_bin_exists = os.path.isfile(fw_bin_path)

    if not fw_bin_exists:
        waflib.Logs.pprint('RED', ('Error: BIN not found at expected location {}, '
                                   'have you run `waf build` yet?'.format(fw_bin_path)))
        return

    # Check if firmware has been bundled
    version_string, version_ts, _ = _get_version_info(ctx)
    fw_type = 'qemu' if ctx.env.QEMU else 'normal'
    fw_pbz_path = ctx.get_pbz_node(fw_type, ctx.env.BOARD, version_string).abspath()
    fw_pbz_exists = os.path.isfile(fw_pbz_path)

    if not fw_pbz_exists:
        waflib.Logs.pprint('CYAN', ('Warning: PBZ not found at expected location {}, '
                                    'running `waf bundle`...').format(fw_pbz_path))

    bundle(ctx)

    # Run power tests using remote_runner.py
    remote_runner_path = os.path.join(pebblesdk_test_root, 'remote_runner.py')
    if not os.path.isfile(remote_runner_path):
        waflib.Logs.pprint('RED', ('Error: remote_runner.py not found in {}. '
                                   'Are you sure that PEBBLESDK_TEST_ROOT is defined correctly?'
                                   .format(pebblesdk_test_root)))
        return
    subprocess.call([remote_runner_path, '--pbz', fw_pbz_path, '[%s]' % suite])


class power_test(BuildContext):
    cmd = 'power_test'

    def execute_build(ctx):
        _run_remote_suite(ctx, 'power')


class integration_test(BuildContext):
    cmd = 'integration_test'

    def execute_build(ctx):
        _run_remote_suite(ctx, 'tintin_3x')


def stop_build_timer(ctx):
    t = datetime.datetime.utcnow() - ctx.pbl_build_start_time
    node = ctx.path.get_bld().make_node('build_time')
    with open(node.abspath(), 'w') as fout:
        fout.write(str(int(round(t.total_seconds()))))


def build(bld):
    bld.DYNAMIC_RESOURCES = []
    bld.LOGHASH_DICTS = []

    # Start this timer here to include the time to generate tasks.
    bld.pbl_build_start_time = datetime.datetime.utcnow()
    bld.add_post_fun(stop_build_timer)

    if bld.variant in ('test', 'test_rocky_emx', 'applib'):
        bld.set_env(bld.all_envs['local'])

    bld.load('file_name_c_define', tooldir='waftools')

    bld.recurse('platform')
    bld.recurse('src/idl')

    if bld.cmd == 'install':
        raise Exception("install isn't a supported command. Did you mean flash?")

    if bld.variant == 'pdc2png':
        bld.recurse('src/libutil')
        bld.recurse('tools')
        return

    if bld.variant == 'tools':
        bld.recurse('tools')
        return

    if bld.variant in ('', 'applib', 'prf'):
        # Dependency for SDK
        bld.recurse('src/fw/vendor/jerryscript')

    if bld.variant == '':
        # sdk generation
        bld.recurse('sdk')

    if bld.variant == 'applib':
        bld.recurse('resources')
        bld.recurse('src/libutil')
        bld.recurse('src/fw')
        bld.recurse('src/fw/vendor/nanopb')
        bld.recurse('src/include')
        bld.recurse('applib-targets')
        return

    if bld.options.onlysdk:
        # stop here, sdk generation is done
        return

    # Do not enable stationary mode in PRF or release firmware
    if (bld.variant != 'prf' and not bld.env.QEMU and bld.env.NORMAL_SHELL != 'sdk'):
        bld.env.append_value('DEFINES', 'STATIONARY_MODE')

    if bld.variant == 'prf':
        bld.set_env(bld.all_envs['arm_prf_mode'])
    elif bld.variant == 'test':
        if bld.env.APPLIB_TARGET == 'emscripten':
            bld.fatal('Did you mean ./waf test_rocky_emx ?')
        bld.recurse('src/include')
        bld.recurse('src/fw/vendor/jerryscript')
        bld.recurse('src/fw/vendor/nanopb')
        bld.recurse('src/libbtutil')
        bld.recurse('src/libos')
        bld.recurse('src/libutil')
        bld.recurse('tools')
        bld.recurse('tests')
        return
    elif bld.variant == 'test_rocky_emx':
        if bld.env.APPLIB_TARGET != 'emscripten':
            bld.fatal('Make sure to ./waf configure with --target=emscripten')
        bld.recurse('src/libutil')
        bld.recurse('src/libos')
        bld.recurse('src/fw/vendor/jerryscript')
        bld.recurse('src/fw/vendor/nanopb')
        bld.recurse('applib-targets')
        bld.recurse('tools')
        bld.recurse('tests')
        return

    if bld.variant == '':
        bld.recurse('stored_apps')

    bld.recurse('src/include')
    bld.recurse('src/libbtutil')
    bld.recurse('src/bluetooth-fw')
    bld.recurse('src/libc')
    bld.recurse('src/libos')
    bld.recurse('src/libutil')
    bld.recurse('src/fw')

    bld.recurse('tools/qemu_spi_cooker')

    # Generate resources. Leave this until the end so we collect all the env['DYNAMIC_RESOURCES']
    # values that the other build steps added.
    bld.recurse('resources')

    # if we're not linking the firmware don't run these
    if not bld.env.NO_LINK:
        bld.add_post_fun(size_fw)
        bld.add_post_fun(size_resources)
        if 'PBL_LOGS_HASHED' in bld.env.DEFINES:
            bld.add_post_fun(merge_loghash_dicts)


class build_prf(BuildContext):
    """executes the recovery firmware build"""
    cmd = 'build_prf'
    variant = 'prf'


class build_applib(BuildContext):
    cmd = 'build_applib'
    variant = 'applib'


def merge_loghash_dicts(bld):
    loghash_dict = bld.path.get_bld().make_node(LOGHASH_OUT_PATH)

    import log_hashing.newlogging
    log_hashing.newlogging.merge_loghash_dict_json_files(loghash_dict, bld.LOGHASH_DICTS)


class SizeFirmware(BuildContext):
    cmd = 'size_fw'
    fun = 'size_fw'

def size_fw(ctx):
    """prints size information of the firmware"""

    fw_elf = ctx.get_tintin_fw_node().change_ext('.elf')
    if fw_elf is None:
        ctx.fatal('No fw ELF found for size')

    fw_bin = ctx.get_tintin_fw_node()
    if fw_bin is None:
        ctx.fatal('No fw BIN found for size')

    import binutils
    text, data, bss = binutils.size(fw_elf.abspath())
    total = text + data
    output = ('{:>7}    {:>7}    {:>7}    {:>7}    {:>7} filename\n'
              '{:7}    {:7}    {:7}    {:7}    {:7x} tintin_fw.elf'.
              format('text', 'data', 'bss', 'dec', 'hex', text, data, bss, total, total))
    Logs.pprint('YELLOW', '\n' + output)

    try:
        space_left = _check_firmware_image_size(ctx, fw_bin.path_from(ctx.path))
    except FirmwareTooLargeException as e:
        if ctx.env.MICRO_FAMILY == 'STM32F2' and ctx.env.QEMU:
            # Let us off with a warning for now
            Logs.warn(str(e))
        else:
            ctx.fatal(str(e))
    else:
        Logs.pprint('CYAN', 'FW: ' + space_left)


class SizeResources(BuildContext):
    cmd = 'size_resources'
    fun = 'size_resources'


def size_resources(ctx):
    """prints size information of resources"""

    if ctx.variant == 'prf':
        return

    pbpack_path = ctx.path.get_bld().find_node('system_resources.pbpack')
    if pbpack_path is None:
        ctx.fatal('No resource pbpack found')

    if ctx.env.MICRO_FAMILY == 'STM32F4':
        max_size = 512 * 1024
    elif ctx.env.MICRO_FAMILY == 'STM32F7':
        max_size = 1024 * 1024
    else:
        max_size = 256 * 1024

    pbpack_actual_size = os.path.getsize(pbpack_path.path_from(ctx.path))
    bytes_free = max_size - pbpack_actual_size

    from waflib import Logs
    Logs.pprint('CYAN', 'Resources: %d/%d (%d free)\n' % (pbpack_actual_size, max_size, bytes_free))

    if pbpack_actual_size > max_size:
        ctx.fatal('Resources are too large for target board %d > %d'
                  % (pbpack_actual_size, max_size))


def size(ctx):
    from waflib import Options
    Options.commands = ['size_fw', 'size_resources'] + Options.commands


class size_prf(BuildContext):
    """checks the size of PRF"""
    cmd = 'size_prf'
    variant = 'prf'


class test(BuildContext):
    """builds and runs the tests"""
    cmd = 'test'
    variant = 'test'


class test_rocky_emx(BuildContext):
    """builds and runs the tests"""
    cmd = 'test_rocky_emx'
    variant = 'test_rocky_emx'


def docs(ctx):
    """builds the documentation out to build/doxygen"""
    ctx.exec_command('doxygen Doxyfile', stdout=None, stderr=None)


class DocsSdk(BuildContext):
    """builds the sdk documentation out to build/sdk/<platformname>/doxygen_sdk"""
    cmd = 'docs_sdk'
    fun = 'docs_sdk'


def docs_sdk(ctx):
    pebble_sdk = ctx.path.get_bld().make_node('sdk')
    supported_platforms = pebble_sdk.listdir()

    for platform in supported_platforms:
        doxyfile = pebble_sdk.find_node(platform).find_node('Doxyfile-SDK.auto')
        if doxyfile:
            ctx.exec_command('doxygen {}'.format(doxyfile.path_from(ctx.path)),
                             stdout=None, stderr=None)


def docs_all(ctx):
    """builds the documentation with all dependency graphs out to build/doxygen"""
    ctx.exec_command('doxygen Doxyfile-all-graphs', stdout=None, stderr=None)

# Bundle commands
#################################################


def _get_version_info(ctx):
    # FIXME: it's probably a better idea to lift board + version info from the .bin file... this can get out of sync!
    git_revision = waftools.gitinfo.get_git_revision(ctx)
    if git_revision['TAG'] != '?':
        version_string = git_revision['TAG']
        version_ts = int(git_revision['TIMESTAMP'])
        version_commit = git_revision['COMMIT']
    else:
        version_string = 'dev'
        version_ts = 0
        version_commit = ''
    return version_string, version_ts, version_commit


def _make_bundle(ctx, fw_bin_path, fw_type='normal', board=None, resource_path=None, write=True):
    import mkbundle

    if board is None:
        board = ctx.env.BOARD

    b = mkbundle.PebbleBundle()

    version_string, version_ts, version_commit = _get_version_info(ctx)
    out_file = ctx.get_pbz_node(fw_type, ctx.env.BOARD, version_string).path_from(ctx.path)

    try:
        _check_firmware_image_size(ctx, fw_bin_path)
        b.add_firmware(fw_bin_path, fw_type, version_ts, version_commit, board, version_string)
    except FirmwareTooLargeException as e:
        ctx.fatal(str(e))
    except mkbundle.MissingFileException as e:
        ctx.fatal('Error: Missing file ' + e.filename + ', have you run ./waf build yet?')

    if resource_path is not None:
        b.add_resources(resource_path, version_ts)
    if 'RELEASE' not in ctx.env.DEFINES and 'PBL_LOGS_HASHED' in ctx.env.DEFINES:
        loghash_dict = ctx.path.get_bld().make_node(LOGHASH_OUT_PATH).abspath()
        b.add_loghash(loghash_dict)

    # Add a LICENSE.txt file
    b.add_license('LICENSE.txt')

    # make sure ctx.capability is available
    ctx.recurse('platform', mandatory=False)

    if ctx.capability('HAS_JAVASCRIPT'):
        js_tooling = ctx.path.get_bld().find_node('src/fw/vendor/jerryscript/js_tooling/js_tooling.js')
        if js_tooling is not None:
            b.add_jstooling(js_tooling.path_from(ctx.path), ctx.capability('JAVASCRIPT_BYTECODE_VERSION'))

    if fw_type == 'normal':
        layouts_node = ctx.path.get_bld().find_node('resources/layouts.json.auto')
        if layouts_node is not None:
            b.add_layouts(layouts_node.path_from(ctx.path))

    if write:
        b.write(out_file)
        waflib.Logs.pprint('CYAN', 'Writing bundle to: %s' % out_file)

    return b


class BundleCommand(BuildContext):
    cmd = 'bundle'
    fun = 'bundle'


def bundle(ctx):
    """bundles a firmware"""

    if ctx.env.QEMU:
        bundle_qemu(ctx)
    else:
        _make_bundle(ctx, ctx.get_tintin_fw_node().path_from(ctx.path),
                     resource_path=ctx.get_pbpack_node().path_from(ctx.path))
        _generate_release_notes(ctx)


def _generate_release_notes(ctx):
    def _write_tag_to_release_notes(task):
        output = task.outputs[0].abspath()
        tag = task.generator.version_tag
        with open(output, 'w') as f:
            f.write(tag)
        task.dep_vars = tag

    git_revision = waftools.gitinfo.get_git_revision(ctx)
    version = "{}.{}".format(git_revision['MAJOR_VERSION'], git_revision['MINOR_VERSION'])
    version_hotfix = "{}.{}".format(version, git_revision['PATCH_VERSION'])
    summary_node = ctx.path.find_node('release-notes').find_node('summary-{}.txt'.format(version_hotfix))
    if summary_node is None:
        summary_node = ctx.path.find_node('release-notes').find_node('summary-{}.txt'.format(version))
    if summary_node is not None:
        ctx(rule='cp ${SRC} ${TGT}',
            source=summary_node,
            target=ctx.path.get_bld().make_node('release-notes.txt'))
    else:
        ctx(rule=_write_tag_to_release_notes,
            version_tag=git_revision['TAG'],
            target=ctx.path.get_bld().make_node('release-notes.txt'))


class bundle_prf(BuildContext):
    """bundles a recovery firmware"""
    cmd = 'bundle_prf'
    variant = 'prf'

    def execute_build(ctx):
        _make_bundle(ctx, ctx.get_tintin_fw_node().path_from(ctx.path), fw_type='recovery')


def _bundle_resourceless_fw(ctx, fw_path, fw_type):
    # We need to create a dummy pbpack and bundle it in. Some FW images don't use
    # resources, but the firmware will refuse to upgrade to a firmware if a resource
    # file isn't sent over, regardless of it's validity.
    import tempfile

    # We need to actually write some content in here or else the phone app won't think the
    # resources are valid, and put_bytes will refuse to update to any firmware image if it doesn't
    # come with a corresponding resource pack. No one will ever read these though so who cares
    # what the content is.
    with tempfile.NamedTemporaryFile(delete=False) as dummy_pbpack:
        dummy_pbpack.write('DUMMY')
        pbpack_path = dummy_pbpack.name

    try:
        _make_bundle(ctx, fw_path, fw_type=fw_type, resource_path=pbpack_path)
    finally:
        os.remove(pbpack_path)


class bundle_recovery(BuildContext):
    """bundles a recovery firmware as normal firmware"""
    cmd = 'bundle_recovery'
    variant = 'prf'

    def execute_build(ctx):
        _bundle_resourceless_fw(ctx, ctx.get_tintin_fw_node().path_from(ctx.path),
                                fw_type='recovery')


class BundleQEMUCommand(BuildContext):
    cmd = 'bundle_qemu'
    fun = 'bundle_qemu'


def bundle_qemu(ctx):
    """bundle QEMU images together into a "fake" PBZ"""

    qemu_image_micro(ctx)
    qemu_image_spi(ctx)

    b = _make_bundle(ctx, ctx.get_tintin_fw_node().path_from(ctx.path),
                     resource_path=ctx.get_pbpack_node().path_from(ctx.path),
                     write=False, board='qemu_{}'.format(ctx.env.BOARD))

    version_string, _, _ = _get_version_info(ctx)
    qemu_pbz = ctx.get_pbz_node('qemu', ctx.env.BOARD, version_string)
    out_file = qemu_pbz.path_from(ctx.path)

    with zipfile.ZipFile(out_file, 'w', compression=zipfile.ZIP_DEFLATED) as pbz_file:
        pbz_file.writestr('manifest.json', json.dumps(b.bundle_manifest))

        files = [ctx.get_tintin_fw_node(),
                 ctx.get_pbpack_node(),
                 'qemu_micro_flash.bin',
                 'qemu_spi_flash.bin']
        if 'PBL_LOGS_HASHED' in ctx.env.DEFINES:
            files.append(LOGHASH_OUT_PATH)

        for fitem in files:
            if isinstance(fitem, Node.Node):
                fnode = fitem
            else:
                fnode = ctx.path.get_bld().make_node(fitem)
            img_path = fnode.path_from(ctx.path)
            pbz_file.write(img_path, os.path.basename(img_path))

    waflib.Logs.pprint('CYAN', 'Writing bundle to: %s' % out_file)

class QemuImageMicroCommand(BuildContext):
    cmd = 'qemu_image_micro'
    fun = 'qemu_image_micro'


class QemuImageMicroPrfCommand(BuildContext):
    cmd = 'qemu_image_prf_micro'
    fun = 'qemu_image_prf_micro'


class QemuImageSpiCommand(BuildContext):
    cmd = 'qemu_image_spi'
    fun = 'qemu_image_spi'


class MfgImageSpiCommand(BuildContext):
    cmd = 'mfg_image_spi'
    fun = 'mfg_image_spi'


def qemu_image_micro(ctx):
    fw_hex = ctx.get_tintin_fw_node().change_ext('.hex')
    _create_qemu_image_micro(ctx, fw_hex.path_from(ctx.path))


def qemu_image_prf_micro(ctx):
    fw_hex = ctx.get_tintin_fw_node_prf().change_ext('.hex')
    _create_qemu_image_micro(ctx, fw_hex.path_from(ctx.path))


def _create_qemu_image_micro(ctx, path_to_firmware_hex):
    """creates the micro-flash image for qemu"""
    from intelhex import IntelHex

    if not ctx.env.BOOTLOADER_HEX:
        ctx.fatal('Board "{}" does not have a bootloader binary available'
                  .format(ctx.env.BOARD))

    micro_flash_node = ctx.path.get_bld().make_node('qemu_micro_flash.bin')
    micro_flash_path = micro_flash_node.path_from(ctx.path)
    waflib.Logs.pprint('CYAN', 'Writing micro flash image to {}'.format(micro_flash_path))

    img = IntelHex(ctx.env.BOOTLOADER_HEX)
    img.merge(IntelHex(path_to_firmware_hex), overlap='replace')

    # Write firmware image and pad up to next 512 byte multiple. This is because QEMU
    # assumes all block devices are multiples of 512 byte sectors
    img.padding = 0xff
    flash_end = ((img.maxaddr() + 511) // 512) * 512
    img.tobinfile(micro_flash_path, start=0x08000000, end=flash_end-1)


def _create_spi_flash_image(ctx, name):
    spi_flash_node = ctx.path.get_bld().make_node(name)
    spi_flash_path = spi_flash_node.path_from(ctx.path)
    waflib.Logs.pprint('CYAN', 'Writing SPI flash image to {}'.format(spi_flash_path))
    return spi_flash_path

def qemu_image_spi(ctx):
    """creates a SPI flash image for qemu"""
    if ctx.env.BOARD.startswith('silk'):
        resources_begin = 0x100000
        image_size = 0x800000
    elif ctx.env.BOARD.startswith('robert') or ctx.env.BOARD.startswith('cutts'):
        resources_begin = 0x200000
        image_size = 0x1000000
    elif ctx.env.MICRO_FAMILY == 'STM32F4':
        resources_begin = 0x380000
        image_size = 0x1000000
    else:
        resources_begin = 0x280000
        image_size = 0x400000

    spi_flash_path = _create_spi_flash_image(ctx, 'qemu_spi_flash.bin')
    with open(spi_flash_path, 'wb') as qemu_spi_img_file:
        # Pad the first section before system resources with FF's'
        qemu_spi_img_file.write("\xff" * resources_begin)

        # Write system resources:
        pbpack = ctx.get_pbpack_node()
        res_img = open(pbpack.path_from(ctx.path), 'rb').read()
        qemu_spi_img_file.write(res_img)

        # Pad with 0xFF up to image size
        tail_padding_size = image_size - resources_begin - len(res_img)
        qemu_spi_img_file.write("\xff" * tail_padding_size)

    with open(os.devnull, 'w') as null:
        qemu_spi_cooker_node = ctx.path.get_bld().make_node('qemu_spi_cooker')
        qemu_spi_cooker_path = qemu_spi_cooker_node.path_from(ctx.path)
        subprocess.check_call([qemu_spi_cooker_path, spi_flash_path], stdout=null)


def mfg_image_spi(ctx):
    """Creates a SPI flash image of PRF for MFG pre-burn. Includes a
    FirmwareDescription struct"""
    import insert_firmware_descr

    if ctx.env.BOARD.startswith('silk'):
        prf_begin = 0x200000
        image_size = 0x800000
    else:
        ctx.fatal("MFG Image not suppored for board: {}".format(ctx.env.BOARD))

    spi_flash_path = _create_spi_flash_image(ctx, 'mfg_prf_image.bin')
    mfg_spi_img_file = open(spi_flash_path, 'wb')

    # Pad the first section before PRF storage
    mfg_spi_img_file.write("\xff" * prf_begin)

    prf_path = ctx.get_tintin_fw_node_prf().path_from(ctx.path)
    prf_image = insert_firmware_descr.insert_firmware_description_struct(prf_path)
    mfg_spi_img_file.write(prf_image)

    # Pad with 0xff up to image size
    tail_padding_size = image_size - prf_begin - len(prf_image)
    mfg_spi_img_file.write("\xff" * tail_padding_size)

def show_ttys(ctx):
    """Displays all available ftdi ports connected to computer"""
    os.system("python ./tools/log_hashing/miniterm_co.py ftdi:///?")


class ConsoleCommand(BuildContext):
    cmd = 'console'
    fun = 'console'


def console(ctx):
    """Starts miniterm with the serial console."""
    # miniterm is not made to be used as a python module, so just shell out:
    tty = ctx.options.tty or _get_dbgserial_tty()

    if _is_pulse_everywhere(ctx):
        os.system("python ./tools/pulse_console.py -t %s" % tty)
    else:
        baudrate = ctx.options.baudrate or 230400
        os.system("python ./tools/log_hashing/miniterm_co.py %s %d" % (tty, baudrate))


class ConsoleCommand(BuildContext):
    cmd = 'console_prf'
    fun = 'console_prf'

def console_prf(ctx):
    os.putenv("PBL_CONSOLE_DICT_PATH", "build/prf/src/fw/loghash_dict.json")
    console(ctx)


class BleConsoleCommand(BuildContext):
    cmd = 'ble_console'
    fun = 'ble_console'


def ble_console(ctx):
    def _get_ble_tty():
        import pebble_tty
        tty = pebble_tty.find_ble_tty()

        if tty is None:
            return None

        waflib.Logs.pprint('GREEN', 'No --tty argument specified, auto-selecting: %s' % tty)
        return tty

    """Starts miniterm with the serial console for the BLE chip."""
    ctx.recurse('platform', mandatory=False)

    # FIXME: We have the ability to program PIDs into the new round of Big Boards. TTY
    # path discovery should be able to use that (PBL-31111). For now, just make a best
    # guess at what the path should be

    if ctx.is_silk() or ctx.is_robert():
        tty_path = _get_ble_tty()
    # if the bt_controller was chosen explicitly, assume we are using an eval board, which
    # happens to match the path for cutts
    elif ctx.uses_dialog_bluetooth():
        tty_path = "ftdi://ftdi:2232:1/1"
    else:
        waflib.Logs.pprint('CYAN', 'Note: This platform does not have a BLE UART')
        tty_path = _get_dbgserial_tty()

    tty = ctx.options.tty or tty_path
    baudrate = ctx.options.baudrate or 230400

    os.system("python ./tools/log_hashing/miniterm_co.py %s %d" % (tty, baudrate))


class BleConsolePrfCommand(BuildContext):
    cmd = 'ble_console_prf'
    fun = 'ble_console_prf'


def ble_console_prf(ctx):
    os.putenv("PBL_CONSOLE_DICT_PATH", "build/prf/src/fw/loghash_dict.json")
    ble_console(ctx)


def accessory_console(ctx):
    def _get_accessory_tty():
        import pebble_tty
        tty = pebble_tty.find_accessory_tty()

        if tty is None:
            return None

        waflib.Logs.pprint('GREEN', 'No --tty argument specified, auto-selecting: %s' % tty)
        return tty

    """Starts miniterm with the accessory connector console."""
    # miniterm is not made to be used as a python module, so just shell out:
    tty = ctx.options.tty or _get_accessory_tty()
    baudrate = ctx.options.baudrate or 115200
    os.system("python ./tools/log_hashing/miniterm_co.py %s %d" % (tty, baudrate))


def qemu(ctx):
    # Make sure the micro-flash image is up to date. By default, we don't rebuild the
    # SPI flash image in case you want to continue with the stored apps, etc. you had before.
    from waflib import Options
    Options.commands = ['qemu_image_micro', 'qemu_launch'] + Options.commands


def qemu_prf(ctx):
    # Make sure the micro-flash image is up to date. By default, we don't rebuild the
    # SPI flash image in case you want to continue with the stored apps, etc. you had before.
    from waflib import Options
    Options.commands = ['qemu_image_prf_micro', 'qemu_launch'] + Options.commands


class QemuLaunchCommand(BuildContext):
    cmd = 'qemu_launch'
    fun = 'qemu_launch'


def qemu_launch(ctx):
    """Starts up the emulator (qemu) """
    ctx.recurse('platform', mandatory=False)

    qemu_machine = ctx.get_qemu_machine()
    if not qemu_machine or qemu_machine == 'unknown':
        raise Exception("Board type '{}' not supported by QEMU".format(ctx.env.BOARD))

    qemu_micro_flash = ctx.path.get_bld().make_node('qemu_micro_flash.bin')
    qemu_spi_flash = ctx.path.get_bld().make_node('qemu_spi_flash.bin')
    qemu_spi_type = ctx.get_qemu_extflash_device_type()
    if not qemu_spi_type:
        raise Exception("External flash type for '{}' not specified".format(ctx.env.BOARD))

    machine_dep_args = ['-machine', qemu_machine,
                        '-cpu', ctx.get_qemu_cpu(),
                        '-pflash', qemu_micro_flash.path_from(ctx.path),
                        qemu_spi_type, qemu_spi_flash.path_from(ctx.path)]

    if ctx.has_touch():
        machine_dep_args.append('-show-cursor')

    cmd_line = (
        "qemu-system-arm -rtc base=localtime "
        "-monitor stdio "
        "-s "
        "-serial file:uart1.log "
        "-serial tcp::12344,server,nowait "   # Used for bluetooth data
        "-serial tcp::12345,server,nowait "   # Used for console
        ) + ' '.join(machine_dep_args)
    os.system(cmd_line)


class QEMUConsoleCommand(BuildContext):
    cmd = 'qemu_console'
    fun = 'qemu_console'


def qemu_console(ctx):
    """Starts miniterm configured to talk to the emulator (qemu)"""
    # miniterm is not made to be used as a python module, so just shell out:
    host_port = ctx.options.qemu_host or 'localhost:12345'

    # A hacky way to pass an argument
    if _is_pulse_everywhere(ctx):
        os.system("python ./tools/pulse_console.py -t %s" % ('socket://%s' % (host_port)))
    else:
        os.system("python ./tools/log_hashing/miniterm_co.py %s" % ('socket://%s' % (host_port)))


class QemuGdb(BuildContext):
    """Starts up a gdb instance to talk to the emulator """
    cmd = 'qemu_gdb'
    fun = 'qemu_gdb'


def qemu_gdb(ctx):
    # First, startup the gdb proxy
    cmd_line = "python ./tools/qemu/qemu_gdb_proxy.py --port=1233 --target=localhost:1234"
    proc = pexpect.spawn(cmd_line, logfile=sys.stdout)
    proc.expect(["Connected to target", pexpect.TIMEOUT], timeout=10)
    fw_elf = ctx.get_tintin_fw_node().change_ext('.elf')
    run_arm_gdb(ctx, fw_elf, target_server_port=1233)


class QemuGdbBoot(BuildContext):
    """ Starts up a gdb instance to talk to the emulator's boot ROM """
    cmd = 'qemu_gdb_boot'
    fun = 'qemu_gdb_boot'


def qemu_gdb_boot(ctx):
    boot_elf = ctx.get_tintin_boot_node().change_ext('.elf')
    run_arm_gdb(ctx, boot_elf, target_server_port=1234)


class debug(BuildContext):
    """ Alias for gdb """
    cmd = 'debug'

    def execute_build(ctx):
        gdb(ctx)


class Gdb(BuildContext):
    """ Starts GDB and openocd (if not already running) and attaches GDB to
        openocd's GDB server. If openocd is already running, it will be used.
    """
    cmd = 'gdb'
    fun = 'gdb'


def gdb(ctx, fw_elf=None, cfg_file='openocd.cfg', is_ble=False):
    if fw_elf is None:
        fw_elf = ctx.get_tintin_fw_node().change_ext('.elf')
    with waftools.openocd.daemon(ctx, cfg_file,
                                 use_swd=(is_ble or 'swd' in ctx.env.JTAG)):
        run_arm_gdb(ctx, fw_elf, cmd_str='--init-command=".gdbinit"')


class gdb_prf(BuildContext):
    """same as `gdb`, but loading the PRF elf instead"""
    cmd = 'gdb_prf'

    def execute_build(ctx):
        gdb(ctx, ctx.get_tintin_fw_node_prf().change_ext('.elf'))


def openocd(ctx):
    """ Starts openocd and leaves it running. It will reset the board to
        increase the chances of attaching succesfully. """
    waftools.openocd.run_command(ctx, 'init; reset', shutdown=False)


# Image commands
#################################################

def _get_dbgserial_tty():
    import pebble_tty
    tty = pebble_tty.find_dbgserial_tty()

    if tty is None:
        return None

    waflib.Logs.pprint('GREEN', 'No --tty argument specified, auto-selecting: %s' % tty)
    return tty


class ble_send_hci(BuildContext):
    """Puts MCU in HCI bypass mode. Sends specified HCI Command and returns result. i.e:
       ./waf send_hci 0x01 0x03 0x0C 0x00
    """
    cmd = 'ble_send_hci'
    fun = 'ble_send_hci'


def ble_send_hci(ctx):
    import prompt
    import pebble_tty
    from serial_port_wrapper import SerialPortWrapper
    import struct
    from time import sleep
    from waflib import Options

    def _dump_hex_array(prefix, hex_array):
        print prefix + " [",
        for i in range(0, len(hex_array)):
            print "0x%02x " % hex_array[i],
        print "]"

    hci_bytes = [int(i, 16) for i in Options.commands]
    _dump_hex_array("Sent HCI CMD:", hci_bytes)

    try:
        device_tty = pebble_tty.find_dbgserial_tty()
        serial = SerialPortWrapper(device_tty)

        prompt.go_to_prompt(serial)
        prompt.issue_command(serial, "bt test hcipass")
        sleep(0.1)

        serial.write_fast(struct.pack('B'*len(hci_bytes), *hci_bytes))

        response = serial.read()
        response = struct.unpack('%dB' % len(response), response)

        serial.write(struct.pack('B', 0x04))  # issue ctrl-d

        _dump_hex_array(" Got HCI EVT:", response)
    finally:
        # note: random bytes get dropped on subsequent usb ops if you forget to close!
        serial.close()

    # WAF/optparse does not have native support for adding sub-command options
    # or variable length options. Reset the options list to prevent innocuous
    # messaging about unrecognized commands
    Options.commands = []
    return None


class ImageResources(BuildContext):
    """flashes resources"""
    cmd = 'image_resources'
    fun = 'image_resources'


def _is_pulse_everywhere(ctx):
    return "PULSE_EVERYWHERE=1" in ctx.env["DEFINES"]


def _get_pulse_flash_tool(ctx):
    if _is_pulse_everywhere(ctx):
        return "pulse_flash_imaging"
    else:
        return "pulse_legacy_flash_imaging"


def image_resources(ctx):
    tty = ctx.options.tty or _get_dbgserial_tty()
    if tty is None:
        waflib.Logs.pprint('RED', 'Error: --tty not specified')
        return

    tool_name = _get_pulse_flash_tool(ctx)
    pbpack_path = ctx.get_pbpack_node().abspath()
    waflib.Logs.pprint('CYAN', 'Writing pbpack "%s" to tty %s' % (pbpack_path, tty))

    ret = os.system("python ./tools/%s.py -t %s -p resources %s" % (tool_name, tty, pbpack_path))
    if ret != 0:
        ctx.fatal('Imaging failed')


class ImageRecovery(BuildContext):
    """flashes recovery firmware"""
    cmd = 'image_recovery'
    fun = 'image_recovery'


def image_recovery(ctx):
    tty = ctx.options.tty or _get_dbgserial_tty()
    if tty is None:
        waflib.Logs.pprint('RED', 'Error: --tty not specified')
        return

    tool_name = _get_pulse_flash_tool(ctx)
    recovery_bin_path = ctx.options.file or ctx.get_tintin_fw_node_prf().path_from(ctx.path)
    waflib.Logs.pprint('CYAN', 'Writing recovery bin "%s" to tty %s' % (recovery_bin_path, tty))

    ret = os.system("python ./tools/%s.py -t %s -p firmware %s" % (tool_name, tty, recovery_bin_path))
    if ret != 0:
        ctx.fatal('Imaging failed')


# Flash commands
#################################################

class FirmwareTooLargeException(Exception):
    pass


def _check_firmware_image_size(ctx, path):
    BYTES_PER_K = 1024
    firmware_size = os.path.getsize(path)
    # Determine flash and bootloader size so we can calculate the max firmware size
    if ctx.env.MICRO_FAMILY == 'STM32F2':
        # 512k of flash and 16k bootloader
        max_firmware_size = (512 - 16) * BYTES_PER_K
    elif ctx.env.MICRO_FAMILY == 'STM32F4':
        if ctx.env.BOARD.startswith('silk') and ctx.variant == 'prf':
            # silk PRF is limited to 512k to save on SPI flash space
            max_firmware_size = 512 * BYTES_PER_K
        elif ctx.env.BOARD in ('snowy_evt', 'snowy_evt2', 'spalding_evt'):
            # 1024k of flash and 64k bootloader
            max_firmware_size = (1024 - 64) * BYTES_PER_K
        else:
            # 1024k of flash and 16k bootloader
            max_firmware_size = (1024 - 16) * BYTES_PER_K
    elif ctx.env.MICRO_FAMILY == 'STM32F7':
        if ctx.variant == 'prf' and not ctx.env.IS_MFG:
            # Robert PRF is limited to 512k to save on SPI flash space
            max_firmware_size = 512 * BYTES_PER_K
        else:
            # 2048k of flash and 32k bootloader
            max_firmware_size = (2048 - 32) * BYTES_PER_K
    else:
        ctx.fatal('Cannot check firmware size against unknown micro family "{}"'
                  .format(ctx.env.MICRO_FAMILY))

    if firmware_size > max_firmware_size:
        raise FirmwareTooLargeException('Firmware is too large! Size is 0x%x should be less than 0x%x' \
                                        % (firmware_size, max_firmware_size))

    return ('%d / %d bytes used (%d free)' %
            (firmware_size, max_firmware_size, (max_firmware_size - firmware_size)))


class FlashCommand(BuildContext):
    """alias for flash_everything"""
    cmd = 'flash'
    fun = 'flash'


def flash(ctx):
    flash_everything(ctx, ctx.get_tintin_fw_node())


class FlashPrfCommand(BuildContext):
    """flashes recovery firmware as normal firmware"""
    cmd = 'flash_prf'
    fun = 'flash_prf'


def flash_prf(ctx):
    flash_everything(ctx, ctx.get_tintin_fw_node_prf())


class FlashBootCommand(BuildContext):
    cmd = 'flash_boot'
    fun = 'flash_boot'


def flash_boot(ctx):
    """flashes a bootloader"""
    if not ctx.env.BOOTLOADER_HEX:
        ctx.fatal("Target does not have a bootloader binary available")
    waftools.openocd.run_command(ctx, 'init; reset halt; ' +
                                 'flash write_image erase ' + ctx.env.BOOTLOADER_HEX + '; '
                                 'reset;',
                                 expect=["wrote"],
                                 enforce_expect=True)


class FlashFirmware(BuildContext):
    """flashes a firmware"""
    cmd = 'flash_fw'

    def execute_build(ctx):
        flash_fw(ctx, ctx.get_tintin_fw_node())


def flash_fw(ctx, fw_bin):
    _check_firmware_image_size(ctx, fw_bin.path_from(ctx.path))

    hex_path = fw_bin.change_ext('.hex').path_from(ctx.path)
    waftools.openocd.run_command(ctx, 'init; reset halt; ' +
                                 'flash write_image erase {}; '.format(hex_path) +
                                 'reset;',
                                 expect=["wrote"],
                                 enforce_expect=True)


def flash_everything(ctx, fw_bin):
    """flashes a bootloader and firmware"""
    if ctx.env.QEMU:
        ctx.fatal("I'm sorry Dave, I can't let you do that.\n"
                  "QEMU firmwares do not work on physical hardware.\n"
                  "Configure without --qemu and rebuild before trying again.")

    _check_firmware_image_size(ctx, fw_bin.path_from(ctx.path))

    if not ctx.env.BOOTLOADER_HEX:
        ctx.fatal("Target does not have a bootloader binary available")

    hex_path = fw_bin.change_ext('.hex').path_from(ctx.path)
    waftools.openocd.run_command(ctx, 'init; reset halt; '
                                 'flash write_image erase ' + ctx.env.BOOTLOADER_HEX + ';\n'
                                 'flash write_image erase ' + hex_path + '; '
                                 'reset;',
                                 expect=["wrote", "wrote", "shutdown"],
                                 enforce_expect=True)


def force_flash(ctx):
    """forces a connected device into a flashing state"""
    (is_newer_than_0_7_0, _) = waftools.openocd.get_flavor(ctx)
    reset_config = waftools.openocd._get_reset_conf(ctx, is_newer_than_0_7_0, True)
    reset_cmd = "reset_config %s; " % reset_config
    waftools.openocd.run_command(ctx, reset_cmd + 'init; reset halt;', ignore_fail=True)
    waftools.openocd.run_command(ctx, reset_cmd + 'init; stm32x unlock 0;', ignore_fail=True)


def reset(ctx):
    """resets a connected device"""
    waftools.openocd.run_command(ctx, 'init; reset;', expect=["found"])


def bork(ctx):
    """resets and wipes a connected a device"""
    waftools.openocd.run_command(ctx, 'init; reset halt;', ignore_fail=True)
    waftools.openocd.run_command(ctx, 'init; flash erase_sector 0 0 1;', ignore_fail=True)


def make_lang(ctx):
    """generate translation files and update existing ones"""
    ctx.recurse('resources/normal/base/lang')


class PackLangCommand(BuildContext):
    cmd = 'pack_lang'
    fun = 'pack_lang'


def pack_lang(ctx):
    """generates pbpack for langs"""
    ctx.recurse('resources/normal/base/lang')


class PackAllLangsCommand(BuildContext):
    cmd = 'pack_all_langs'
    fun = 'pack_all_langs'


def pack_all_langs(ctx):
    """generates pbpack for all langs"""
    ctx.recurse('resources/normal/base/lang')


# Tool build commands
#################################################


class build_pdc2png(BuildContext):
    """executes the pdc2png build"""
    cmd = 'build_pdc2png'
    variant = 'pdc2png'


class build_tools(BuildContext):
    """build all tools in tools/ dir"""
    cmd = 'build_tools'
    variant = 'tools'

# vim:filetype=python
