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


from datetime import datetime
import json
import logging
import os
import re
import readline
import shlex
import sys
import threading
import time
import tokenize
import traceback
import types
import unicodedata as ud

import prompt_toolkit

from log_hashing.logdehash import LogDehash
import pulse


class PebbleCommander(object):
    """ Pebble Commander.
        Implements everything for interfacing with PULSE things.
    """

    def __init__(self, tty=None, interactive=False):
        self.connection = pulse.socket.Connection.open_dbgserial(
                url=tty, infinite_reconnect=interactive)
        self.connection.change_baud_rate(921600)
        self.interactive = interactive

        self.log_listeners_lock = threading.Lock()
        self.log_listeners = []

        # Start the logging thread
        self.log_thread = threading.Thread(target=self._start_logging)
        self.log_thread.daemon = True
        self.log_thread.start()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.close()

    def __del__(self):
        self.close()

    @classmethod
    def command(cls, name=None):
        """ Registers a command.
            `name` is the command name. If `name` is unspecified, name will be the function name
            with underscores converted to hyphens.

            The convention for `name` is to separate words with a hyphen. The function name
            will be the same as `name` with hyphens replaced with underscores.
            Example: `click-short` will result in a PebbleCommander.click_short function existing.

            `fn` should return an array of strings (or None), and take the current
            `PebbleCommander` as the first argument, and the rest of the argument strings
            as subsequent arguments. For errors, `fn` should throw an exception.

            # TODO: Probably make the return something structured instead of strongly typed.
        """
        def decorator(fn):
            # Story time:
            # <cory> Things are fine as long as you only read from `name`, but assigning to `name`
            #        creates a new local which shadows the outer scope's variable, even though it's
            #        only assigned later on in the block
            # <cory> You could work around this by doing something like `name_ = name` and using
            #        `name_` in the `decorator` scope
            cmdname = name
            if not cmdname:
                cmdname = fn.__name__.replace('_', '-')
            funcname = cmdname.replace('-', '_')
            if not re.match(tokenize.Name + '$', funcname):
                raise ValueError("command name %s isn't a valid name" % funcname)
            if hasattr(cls, funcname):
                raise ValueError('function name %s clashes with existing attribute' % funcname)
            fn.is_command = True
            fn.name = cmdname
            method = types.MethodType(fn, None, cls)
            setattr(cls, funcname, method)

            return fn
        return decorator

    def close(self):
        try:
            self.connection.close()
        except:
            pass

    def _start_logging(self):
        """ Thread to handle logging messages.
        """
        while True:
            msg = self.connection.logging.receive()
            with self.log_listeners_lock:
                # TODO: Buffer log messages if no listeners attached?
                for listener in self.log_listeners:
                    try:
                        listener(msg)
                    except:
                        pass

    def attach_log_listener(self, listener):
        """ Attaches a listener for log messages.
            Function takes message and returns are ignored.
        """
        with self.log_listeners_lock:
            self.log_listeners.append(listener)

    def detach_log_listener(self, listener):
        """ Removes a listener that was added with `attach_log_listener`
        """
        with self.log_listeners_lock:
            self.log_listeners.remove(listener)

    def send_prompt_command(self, cmd):
        """ Send a prompt command string.
            Unfortunately this is indeed strongly typed, a better solution is necessary.
        """
        return self.connection.prompt.command_and_response(cmd)

    def get_command(self, command):
        try:
            fn = getattr(self, command.replace('-', '_'))
            if fn.is_command:
                return fn
        except AttributeError:
            # Method doesn't exist, or isn't a command.
            pass

        return None


class InteractivePebbleCommander(object):
    """ Interactive Pebble Commander.
        Most/all UI implementations should either use this directly or sub-class it.
    """
    def __init__(self, loghash_path=None, tty=None):
        self.cmdr = PebbleCommander(tty=tty, interactive=True)
        if loghash_path is None:
            loghash_path = "build/src/fw/loghash_dict.json"
        self.dehasher = LogDehash(loghash_path)
        self.cmdr.attach_log_listener(self.log_listener)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.close()

    def __del__(self):
        self.close()

    def close(self):
        try:
            self.cmdr.close()
        except:
            pass

    def attach_prompt_toolkit(self):
        """ Attaches prompt_toolkit things
        """
        self.history = prompt_toolkit.history.InMemoryHistory()
        self.cli = prompt_toolkit.CommandLineInterface(
            application=prompt_toolkit.shortcuts.create_prompt_application(u"> ",
                                                                           history=self.history),
            eventloop=prompt_toolkit.shortcuts.create_eventloop())
        self.patch_context = self.cli.patch_stdout_context(raw=True)
        self.patch_context.__enter__()

    def log_listener(self, msg):
        """ This is called on every incoming log message.
            `msg` is the raw log message class, without any dehashing.

            Subclasses should override this probably.
        """
        line_dict = self.dehasher.dehash(msg)
        line = self.dehasher.commander_format_line(line_dict)
        print line

    def dispatch_command(self, string):
        """ Dispatches a command string.

            Subclasses should not override this.
        """
        args = shlex.split(string)
        # Starting with '!' passes the rest of the line directly to prompt.
        # Otherwise we try to run a command; if that fails, the line goes to prompt.
        if string.startswith("!"):
            string = string[1:]  # Chop off the '!' marker
        else:
            cmd = self.cmdr.get_command(args[0])
            if cmd:  # If we provide the command, run it.
                return cmd(*args[1:])

        return self.cmdr.send_prompt_command(string)

    def input_handle(self, string):
        """ Handles an input line.
            Generally the flow is to handle any UI-specific commands, then pass on to
            dispatch_command.

            Subclasses should override this probably.
        """
        # Handle "quit" strings
        if string in ["exit", "q", "quit"]:
            return False

        try:
            resp = self.dispatch_command(string)
            if resp is not None:
                print "\x1b[1m" + '\n'.join(resp) + "\x1b[m"
        except:
            print "An error occurred!"
            traceback.print_exc()

        return True

    def get_command(self):
        """ Get a command input line.
            If there is no line, return an empty string or None.
            This may block.

            Subclasses should override this probably.
        """
        if self.cli is None:
            self.attach_prompt_toolkit()
        doc = self.cli.run(reset_current_buffer=True)
        if doc:
            return doc.text
        else:
            return None

    def command_loop(self):
        """ The main command loop.

            Subclasses could override this, but it's probably not useful to do.
        """
        while True:
            try:
                cmd = self.get_command()
                if cmd and not self.input_handle(cmd):
                    break
            except (KeyboardInterrupt, EOFError):
                break
