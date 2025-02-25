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

# Handy gdb python extensions
#
# To load these, use the gdb source command:
#  (gdb) source tools/gdb_tintin.py
#
# Or, this script can be setup to auto-load into gdb by placing a link to it into the
#  <data-directory>/python/gdb/command directory
#
# For example:
#   (gdb) show data-directory
#   GDB's data directory is "/Users/foo/arm-cs-tools/share/gdb"
#
#   > cd /Users/foo/arm-cs-tools/share/gdb
#   > ln -s ~/Projects/pebble/tintin/tools/gdb_tintin.py

import collections
import os
import sys

# Necessary to allow the sh import to succeed when this script is auto-loaded by gdb (when placed into the
#  <data-directory>/python/gdb/command directory).
sys.argv = []
import sh

try:
    import gdb
except ImportError:
    raise Exception("This file is a GDB script.\n"
                    "It is not intended to be run outside of GDB.\n"
                    "Hint: to load a script in GDB, use `source this_file.py`")

import gdb.printing
import ctypes
import datetime
import itertools
import re
import struct

from collections import defaultdict, OrderedDict, namedtuple, Counter

# Enable importing of other .py files in the same folder:
sys.path.insert(
        0, os.path.abspath(os.path.dirname(os.path.expanduser(__file__))))

import gdb_utils
import gdb_parser
from gdb_heap import Heap
from gdb_tintin_metadata import TintinMetadata
from gdb_symbols import get_static_variable, get_static_function


class PblCommand(gdb.Command):
    """Pebble-specific commands."""

    def __init__(self):
        super(PblCommand, self).__init__(
                'pbl', gdb.COMMAND_USER, gdb.COMPLETE_NONE, prefix=True)

    def invoke(self, arg, from_tty):
        gdb.execute('help pbl')
PblCommand()


class PebbleTaskBitSet(object):
    """ Convenience class to format a PebbleTask bitset """

    def __init__(self, bitset):
        self.bitset = int(bitset)

        # Populate PebbleTask enum value->string map:
        enum = {}
        enum_fields = gdb.lookup_type("PebbleTask").fields()
        for field in enum_fields:
            value = int(field.enumval)
            enum[value] = str(field.name)
        self.enum = enum

    def human_readable_list(self, separator="\n", indent=" - "):
        task_names = []
        for value in range(0, 32):
            if self.bitset & (1 << value):
                task_name = self.enum[value] if value in self.enum else "???"
                task_names.append(task_name)
        if not task_names:
            return ""
        return indent + (separator + indent).join(task_names)

    def __str__(self):
        return self.human_readable_list()

class FreeRTOSMutex(object):
    """ Convenience class to access FreeRTOS mutex structure """

    def __init__(self, mutex):
        from pprint import pprint
        self.address = mutex
        self.mutex = mutex.cast(gdb.lookup_type("LightMutex_t").pointer())

    def waiters(self, indent=" - "):
        """ Returns a list of the waiters on the mutex provided """
        waiter_name_list = ""

        for waiter_name in self.waiter_task_list():
            waiter_name_list += indent + ("%s\n" % waiter_name)

        return waiter_name_list

    def waiter_task_list(self):
        num_waiters = self.num_waiters()
        waiter_task_list = [ ]
        waiter_list = self.mutex["xTasksWaitingToLock"].cast(gdb.lookup_type("List_t"))
        waiter_list_entry = waiter_list["xListEnd"]["pxNext"]
        while (num_waiters > 0):
            waiter = waiter_list_entry["pvOwner"]

            gdb_waiter_name = waiter.cast(gdb.lookup_type("TCB_t").pointer())
            waiter_name = gdb_waiter_name["pcTaskName"].string()
            waiter_task_list.append(waiter_name)

            num_waiters -= 1
            waiter_list_entry = waiter_list_entry["pxNext"]
        return waiter_task_list

    def num_waiters(self):
        waiter_list = self.mutex["xTasksWaitingToLock"].cast(gdb.lookup_type("List_t"))
        return int(waiter_list["uxNumberOfItems"])

    def owner(self):
        return self.mutex["pxMutexHolder"]

    def owner_name(self):
        task_owner = self.owner()
        if (task_owner != 0):
            task = task_owner.cast(gdb.lookup_type("TCB_t").pointer())
            # convert gdb char array to string
            task_name = task["pcTaskName"].string()
            return task_name
        return 0

    def locked(self):
        return bool(self.mutex["uxLocked"])

class FreeRTOSQueue(object):
    """ Convenience class to access data in a FreeRTOS queue """

    def __init__(self, queue, item_type_or_string=None):
        if type(queue) is str:
            queue = gdb.parse_and_eval(queue)
        self.queue = queue.cast(gdb.lookup_type("Queue_t").pointer())

        if item_type_or_string is None:
            # Fallback to uint8_t * :
            self.item_type = gdb.lookup_type("uint8_t").pointer()
        elif type(item_type_or_string) is str:
            self.item_type = gdb.lookup_type(item_type_or_string).pointer()
        elif type(item_type_or_string) is gdb.Type:
            self.item_type = item_type_or_string
        else:
            raise Exception("Unexpected type: %s" % type(item_type_or_string))

        self.num_waiting = int(self.queue["uxMessagesWaiting"])
        self.item_size = int(self.queue["uxItemSize"])
        self.size = int(self.queue["uxLength"])

    def items(self):
        """ Returns a list with the gdb.Value objects that are queued """

        cursor = self.queue["u"]["pcReadFrom"]
        tail = self.queue["pcTail"]
        items_left = self.num_waiting
        items = []
        while items_left:
            cursor += self.item_size
            if cursor >= tail:
                cursor = self.queue["pcHead"]
            item = cursor.cast(self.item_type)
            items.append(item)
            items_left -= 1
        return items

    def previously_processed_items(self):
        """ Returns a list with the gdb.Value objects that were previously
        executed. Since the FreeRTOS queue is actually an array, older events
        are still sitting around """

        cursor = self.queue["u"]["pcReadFrom"]
        tail = self.queue["pcTail"]
        head = self.queue["pcHead"]
        items_left = self.size - self.num_waiting

        # Find the oldest item
        cursor = cursor + self.num_waiting * self.item_size
        if cursor > tail:
            cursor = head + (cursor - tail)

        items = []
        while items_left:
            cursor += self.item_size
            if cursor >= tail:
                cursor = head
            item = cursor.cast(self.item_type)
            items.append(item)
            items_left -= 1

        return items

    # TODO: this should all really be in a gdb pretty print handler
    def pretty_print_pebble_event_queue(self, items):
        descr = ""
        for item in items:
            descr += "\n<0x%08x> %s " % (item.dereference().address, item["type"])
            probable_type = ""
            prev_underscore = False
            # Attempt to convert PebbleEventType enum into the struct type it corresponds to
            for i, c in enumerate(str(item["type"])):
                if i == 0 or prev_underscore:
                    probable_type += c
                    prev_underscore = False
                elif c == '_':
                    prev_underscore = True
                else:
                    probable_type += c.lower()

            # A few of the enums map to the same underlying type
            if 'Button' in probable_type:
                probable_type = "PebbleButtonEvent"
            elif 'Focus' in probable_type:
                probable_type = "PebbleAppFocusEvent"
            elif 'Blobdb' in probable_type:
                probable_type = "PebbleBlobDBEvent"

            event_type = None
            try:
                event_type = gdb.lookup_type(probable_type)
                descr += str(item.dereference().cast(gdb.lookup_type(probable_type)))
            except:
                descr += "\n\tUnknown Type %s" % probable_type

        return descr

    def get_queue_item_description(self, items):
        descr = "Queue <0x%x> with (%u / %u) items:\n" % \
                (self.queue, len(items), self.size)
        # if self.item_type == gdb.lookup_type("PebbleEvent").pointer():
        #     descr += self.pretty_print_pebble_event_queue(items)
        # else: # generic dump
        deref_items = ["<0x%08x> %s" % (item.dereference().address, item.dereference())
                       for item in items]
        descr += "\n".join(deref_items)
        return descr

    def print_previously_processed_items(self):
        return self.get_queue_item_description(self.previously_processed_items())

    def __str__(self):
        return self.get_queue_item_description(self.items())


class Tasks(object):
    """ Holds information about valid tasks """
    def __init__(self):
        self.valid_tasks = []

        task_handles = gdb.parse_and_eval("g_task_handles")
        self.total = int(gdb.parse_and_eval("NumPebbleTask"))
        for x in xrange(self.total):
            task = task_handles[x].cast(gdb.lookup_type("TCB_t").pointer())
            if task:
                self.valid_tasks.append(task_handles[x])

    def is_valid_task(self, task):
        return task in self.valid_tasks


class LinkedList(object):
    """ Returns an iterable linked list when given a ListNode. """
    def __init__(self, start_ptr):
        self.addresses = []
        node = start_ptr.cast(gdb.lookup_type("ListNode"))
        direction = "next" if node["next"] else "prev"
        prev_direction = "prev" if node["next"] else "next"
        prev_address = 0

        while node.address != 0:
            if node[prev_direction] != prev_address:
                print("Warning: LinkedList {} corrupted? "
                      "Expected {} got {}".format(node.address, prev_address, node[prev_direction]))
                break
            self.addresses.append(node.address)
            prev_address = node.address
            node = node[direction].dereference()

    def __iter__(self):
        return iter(self.addresses)

    def __str__(self):
        return "LinkedList: {}".format(", ".join(str(address) for address in self.addresses))

    def __repr__(self):
        return self.__str__()


class DumpQueue(gdb.Command):
    """Dumps the queue"""
    def __init__(self):
        desc = "Dumps the contents of the specified queue.\n" \
               "Expected command format:\n" \
               "dumpqueue QueueHandle [TYPE_CAST] [-p]"
        super(DumpQueue, self).__init__('dumpqueue', gdb.COMMAND_USER)
        self.parser = gdb_utils.GdbArgumentParser(prog='dumpqueue', description=desc)
        self.parser.add_argument("-p", dest="dump_prev", action="store_true", default=False,
                                 help="dumps previously processed queue items instead of "
                                 "what is waiting to be processed. NOTE: if there are pointers "
                                 "in these events, they may have already been freed")
        self.parser.add_argument("queueinfo", metavar="N", nargs='?', default=None,
                                 help="QueueHandle")
        self.parser.add_argument("typecast", metavar="N", nargs='?', default=None,
                                 help="[TYPE_CAST]")

    def print_usage(self):
        print self.parser.print_usage()

    def invoke(self, unicode_args, from_tty):
        args = self.parser.parse_args(unicode_args)
        if not args.queueinfo:
            self.print_usage()
            return

        freertos_queue = FreeRTOSQueue(args.queueinfo, args.typecast)

        if args.dump_prev:
            print "---Dumping previously processed queue items---\n"
            print freertos_queue.print_previously_processed_items()
        else:
            print str(freertos_queue)
DumpQueue()


class QueueStats(gdb.Command):
    """Dumps the status of various queues used within PebbleOS."""
    def __init__(self):
        super(QueueStats, self).__init__('pbl queuestats', gdb.COMMAND_USER)

    def invoke(self, unicode_args, from_tty):
        def print_line():
            print "~" * 80

        # Dict Format:
        #  Thread Name 1 : [
        #     [ Queue used by thread 1, type of item in queue]
        #     [ Another queue used by thread 1, type of item in queue]
        #  ]
        #  ...
        #  Thread Name N : [ [] ]
        owners_of_queues_to_dump = {
            "KernelMain" : [
                [ get_static_variable("s_kernel_event_queue"), "PebbleEvent" ],
                [ get_static_variable("s_from_kernel_event_queue"), "PebbleEvent" ],
                [ get_static_variable("s_from_app_event_queue"), "PebbleEvent" ],
                [ get_static_variable("s_from_worker_event_queue"), "PebbleEvent" ]
            ],
            "KernelBG" : [
                [ get_static_variable("s_system_task_queue"), "SystemTaskEvent"],
                [ get_static_variable("s_from_app_system_task_queue"), "SystemTaskEvent" ]
            ],
            "App" : [
                [ get_static_variable("s_to_app_event_queue"), "PebbleEvent" ]
            ],
            "Worker": [
                [ get_static_variable("s_to_worker_event_queue"), "PebbleEvent" ]
            ]
        }

        for owner in owners_of_queues_to_dump.iterkeys():
            print "Dumping queue(s) used by %s" % owner
            print_line()
            for queues in owners_of_queues_to_dump[owner]:
                print "\n--Queue Name: %s--\n" % queues[0]
                freertos_queue = FreeRTOSQueue(queues[0], queues[1])
                print str(freertos_queue)
QueueStats()


class PrintList(gdb.Command):
    """Prints a list of ListNode`s."""

    def __init__(self):
        super(PrintList, self).__init__('pl', gdb.COMMAND_USER)

    def invoke(self, unicode_args, from_tty):
        if not unicode_args:
            print "Prints a list of ListNode nodes.\n" \
                  "Expected command format:\n" \
                  "pl LIST_HEAD [TYPE_CAST]"
            return
        split_args = unicode_args.split(" ", 1)
        list_head = split_args[0]
        node_value = gdb.parse_and_eval(list_head)
        list_node_ptr_type = gdb.lookup_type("ListNode").pointer()

        if len(split_args) > 1:
            cast = split_args[1]
            if '*' in cast:
                print "Specify non-pointer type"
                return
            cast_type = gdb.lookup_type(cast).pointer()
            node_value = node_value.cast(cast_type)
        else:
            cast_type = None

        num_nodes = 0
        while node_value:
            num_nodes += 1
            print str(node_value.dereference())
            if cast_type:
                node_value = node_value.cast(list_node_ptr_type)["next"]
                node_value = node_value.cast(cast_type)
            else:
                node_value = node_value["next"]
        print "%u list nodes" % num_nodes

PrintList()


class StackRecover(gdb.Command):
    """ Recover the stack """

    def __init__(self):
        super(StackRecover, self).__init__('pbl stackwizard', gdb.COMMAND_USER)
        desc = "Attempts to recover a backtrace from a corrupted stack (i.e stack overflow)"
        self.parser = gdb_utils.GdbArgumentParser(prog='pbl stackwizard', description=desc)

    def print_usage(self):
        print self.parser.print_usage()

    def invoke(self, unicode_args, from_tty):
        def count_frames():
            tot = 0
            cur_frame = gdb.newest_frame()
            while cur_frame and cur_frame.is_valid():
                tot += 1
                cur_frame = cur_frame.older()
            return tot

        def tcb_addr_current_thread():
            return gdb.selected_thread().ptid[2]

        args = self.parser.parse_args(unicode_args)

        tcb_ptr_type = gdb.lookup_type("TCB_t").pointer()
        uint32_type = gdb.lookup_type("uint32_t")
        uint32_ptr_type = gdb.lookup_type("uint32_t").pointer()

        thread_addr = tcb_addr_current_thread()
        if thread_addr == -1:
            # We need thread info to be loaded for the commands that follow to work
            gdb.execute("info threads")
            thread_addr = tcb_addr_current_thread()

        if thread_addr == 0:
            thread_addr = gdb.parse_and_eval("pxCurrentTCB")

        addr = ("(uint8_t *)0x%x" % thread_addr)
        tcb = gdb.parse_and_eval(addr).cast(tcb_ptr_type)

        pxBottomStackAddr = tcb["pxStack"].cast(uint32_type)

        last_pc_addr = 0

        print "Examining stack starting at 0x%x ..." % pxBottomStackAddr

        # This algorithm is pretty naive. It effectively assumes that the only
        # time a code address is on the stack is because it was what was stored
        # in the lr. In reality a pc may wind up on the stack for many reasons
        # (for example, function pointers). The algo traverses backward through
        # the stack pretending the first instruction address found is the pc &
        # the next is the lr. Since many functions don't take a function pointer as
        # an argument, it usually locks onto something. Once it has found a potential
        # solution, it analyzes how many valid frames exist. If there's a reasonable
        # number of frames the solution is displayed to the user

        for word in range(0, 200):
            stack_addr = pxBottomStackAddr + (4 * word)
            stack_val = stack_addr.cast(uint32_ptr_type).dereference()
            stack_word_pc_info = gdb.find_pc_line(int(stack_val))
            if stack_word_pc_info.symtab is not None:

                if last_pc_addr != 0:
                    lr = stack_val
                    sp = stack_addr - 4
                    sp = sp.cast(uint32_type)

                    gdb.execute("set $lr=0x%x" % lr)
                    gdb.execute("set $sp=0x%x" % sp)

                    # we've found a potential match. Does it give us a reasonable backtrace?
                    if count_frames() > 4:
                        print "Potential backtrace found %d bytes above stack bottom!" % \
                            (last_pc_addr - int(pxBottomStackAddr.cast(uint32_type)))
                        print "~" * 80
                        gdb.execute("backtrace")
                        break

                pc = stack_val
                gdb.execute("set $pc=0x%x" % stack_val)
                last_pc_addr = int(stack_addr.cast(uint32_type))

StackRecover()

class StackStats(gdb.Command):
    """Print Stack Usage by routine"""

    def __init__(self):
        super(StackStats, self).__init__('sbt', gdb.COMMAND_USER)

    def print_usage(self):
        print "Prints a backtrace of the currently selected thread displaying\n" \
            " how much stack each method uses\n"

    def invoke(self, unicode_args, from_tty):
        cur_frame = gdb.newest_frame()
        tot_depth = 0
        if cur_frame.name() is not None:
            print "     - %s" % cur_frame.name()

        while cur_frame and cur_frame.is_valid():
            # Parses the following string to pull out the $sp:
            # {stack=0x20014fb8,code=0x80107d0,!special}
            #
            # TODO: it looks like some versions of the python API have a
            # read_register API but ours does not. We should use it when it
            # becomes available
            regex = r"stack=(\w+),*"
            cur_sp = re.findall(regex, str(cur_frame))

            if len(cur_sp) and cur_frame.older():
                older_sp = re.findall(regex, str(cur_frame.older()))
                if len(older_sp):
                    stack_use = int(older_sp[0], 16) - int(cur_sp[0], 16)
                    tot_depth += stack_use
                    name = cur_frame.older().name()
                    if name is not None:
                        print "%4d - %s" % (stack_use, name)
            cur_frame = cur_frame.older()
        print "Total Stack Depth: %d bytes" % tot_depth
StackStats()


class HeapParser(gdb.Command):
    """ Try to figure out what structures are allocated on the heap """

    def __init__(self):
        super(HeapParser, self).__init__('pbl heap', gdb.COMMAND_USER)
        desc = "Attempts to guess the type of each block in the kernel heap."
        self.parser = gdb_utils.GdbArgumentParser(prog='pbl heap', description=desc)
        self.parser.add_argument("-d", dest="dump_heap", action="store_true", default=False,
                                 help="dump the heap")
        self.parser.add_argument("-s", dest="dump_strings", action="store_true", default=False,
                                 help="dump strings on the heap")
        self.parser.add_argument("-u", dest="dump_unknowns", action="store_true", default=False,
                                 help="dump unknowns on the heap")

    def print_usage(self):
        print self.parser.print_usage()

    def invoke(self, unicode_args, from_tty):
        args = self.parser.parse_args(unicode_args)

        s_kernel_heap = get_static_variable('s_kernel_heap', ref=True)
        heap = Heap(s_kernel_heap)

        data = gdb_parser.parse_heap(heap)

        prefer = ["Semaphore", "AppMessageBuffer", "AnalyticsStopwatch"]

        block_data = dict([(str(block.data), []) for block in heap])
        for name, blocks in data.iteritems():
            addresses = [str(block.data) for block in blocks]
            for address in addresses:
                block_data[address].append(name)

        data["Unknown"] = []
        for block in heap:
            key = str(block.data)
            if not len(block_data[key]):
                data["Unknown"].append(block)
            elif len(block_data[key]) > 1:
                objects = block_data[key]
                deduped = next(([obj] for obj in prefer if obj in objects), objects)
                for obj in objects:
                    if obj not in deduped:
                        data[obj] = [block for block in data[obj] if str(block.data) != key]
                block_data[key] = deduped

        if args.dump_heap:
            print "~" * 60
            for block in heap:
                print "Addr: {}  Bytes: {:<6} {}".format(block.data, block.size,
                                                         block_data[str(block.data)])

        print "~" * 60
        print "Summary:"

        for struct, blocks in OrderedDict(sorted(data.items())).iteritems():
            print "{:<30}: {:<3} ({} bytes)".format(struct, len(blocks),
                                                    sum(block.size for block in blocks))

        if args.dump_strings:
            print "~" * 60
            print "Strings:"

            for block in data["String"]:
                print "Addr: {} -> {}".format(block.data, block.cast("char").string())

            print "Tasks:"
            for block in data["Task"]:
                print "Addr: {} -> Task: {}".format(block.data,
                                                    block.cast("TCB_t")["pcTaskName"].string())

        if args.dump_unknowns:
            print "~" * 60
            print "Unknown:"

            for block in data["Unknown"]:
                desc = ""
                if heap.malloc_instrumentation:
                    pc = int(block.info["pc"])
                    info = gdb_utils.addr2line(pc)
                    desc = "{info.filename}:{info.line}".format(info=info)
                print "Addr: {}  Bytes: {:<8} {}".format(block.data, block.size, desc)
            print "Note: Most unknowns in the kernel heap are from applib_malloc."
        if len(data["Unknown"]) > 20:
            print "Warning: High amount of unknown blocks. Consider adding another parser."
        if len([block for block in data["Unknown"] if block.size < 100]) > 8:
            print "High amount of small unknown blocks. Is an animation onscreen?"

HeapParser()


class LockStats(gdb.Command):
    """Walk through mutexes"""

    def __init__(self):
        super(LockStats, self).__init__('lockstats', gdb.COMMAND_USER)

    def print_usage(self):
        print("Checks for deadlocks and prints warnings if a deadlock \n"
              "has occurred")

    def invoke(self, unicode_args, do_print=True):
        s_kernel_heap = get_static_variable('s_kernel_heap', ref=True)
        heap = Heap(s_kernel_heap)

        data = gdb_parser.parse_heap(heap, ['LightMutex', 'PebbleMutex', 'Semaphore'])

        self.tasks = Tasks()
        message = ["Lock status:\n"]

        # Maps tasks to a list of tasks it's waiting on
        task_lock_wait_dict = defaultdict(list)

        mutexes = [FreeRTOSMutex(block.data) for block
                   in data["LightMutex"]]
        mutexes = [mutex for mutex in mutexes if mutex.owner()]
        message.append("Found %s owned mutexes." % len(mutexes))

        # Search for PebbleMutexes
        pebble_mutexes = [block.data for block in data["PebbleMutex"]]
        mutex_lrs = {str(mutex["freertos_mutex"]): gdb_utils.addr2line(mutex["lr"])
                     for mutex in pebble_mutexes}

        for mutex in mutexes:
            message.append("Mutex Addr: {} Owner: {}".format(mutex.address, mutex.owner_name()))
            message.append("Last locked: {}".format(mutex_lrs.get(str(mutex.address)), 0))

            for waiter in mutex.waiter_task_list():
                message.append("\t{} waiting for lock".expandtabs(2).format(waiter))

                task_lock_wait_dict[waiter].append(mutex.owner_name())
            message.append("")

        for task, locks in task_lock_wait_dict.items():
            for lock in locks:
                # Is the lock waiting for the current task?
                if task in task_lock_wait_dict.get(lock, []):
                    message.append("Found deadlock between {} and {}".format(task, lock))
            del task_lock_wait_dict[task]

        message = "\n".join(message)

        if do_print:
            print message

        return message

LockStats()


class HeapStats(gdb.Command):
    """Print Heap info"""

    def __init__(self):
        super(HeapStats, self).__init__('heapstats', gdb.COMMAND_USER)
        self.parser = gdb_utils.GdbArgumentParser(prog='heapstats', description="Print heap info")
        self.parser.add_argument("heap", help="pointer to the heap")
        self.parser.add_argument("-d", dest="dump_heap", action="store_true", default=False,
                                 help="dump blocks in the heap")
        self.parser.add_argument("-f", dest="dump_freq", action="store_true", default=False,
                                 help="dump the frequency of each block size")
        self.parser.add_argument("-s", dest="dump_size", action="store_true", default=False,
                                 help="dump the size allocated by each file")

    def print_usage(self):
        print self.parser.print_help()

    def invoke(self, unicode_args, from_tty):
        args = self.parser.parse_args(unicode_args)

        self.alloc_segments = []
        self.free_segments = []
        self.heap_size_dict = defaultdict(int)
        self.file_size_list = []

        heap = Heap(args.heap, show_progress=True)
        self.malloc_instrumentation = heap.malloc_instrumentation

        for block in heap:
            self.extract_info(block)

        # Ensure the next line printed is on its own line instead of appended to the progress
        print ""

        if args.dump_heap or args.dump_size:
            if not self.malloc_instrumentation:
                print "Warning: Malloc instrumentation not enabled."
            for block in heap:
                self.extract_file_info(block)

        if len(self.free_segments) == 0:
            self.free_segments = [0]

        if args.dump_size:
            filesize_dict = defaultdict(int)
            for pc, segment_ptr, size_bytes, filename, desc in self.file_size_list:
                if filename:
                    filesize_dict[filename] += size_bytes
                else:
                    filesize_dict["FREE"] += size_bytes

            filesize_dict_sorted = OrderedDict(sorted(filesize_dict.items(),
                key=lambda x: x[1], reverse=True))
            print "File: Heap usage (bytes)"
            for filename, size in filesize_dict_sorted.iteritems():
                print "{}: {}".format(filename, size)

        if args.dump_heap:
            for pc, ptr, size_bytes, filename, desc in self.file_size_list:
                print "PC:0x{:0>8x} Addr:{} Bytes:{:<8} {}".format(pc, ptr, size_bytes, desc)

        if args.dump_freq:
            heap_size_dict = Counter(self.alloc_segments)
            heaps_dict_sorted = OrderedDict(sorted(heap_size_dict.items(),
                key=lambda x: x[1], reverse=True))
            print "Freq: Size (bytes)"
            for size, freq in heaps_dict_sorted.items():
                print "{:>4d}: {}".format(freq, size)

        if heap.corrupted:
            message = "HEAP CORRUPTED!"
            if not args.dump_heap:
                message += " Dump heap for details"
            print message

        class HeapInfo(namedtuple('HeapInfo', 'blocks size max')):
            def __new__(cls, segments):
                return cls._make([len(segments), sum(segments), max(segments)])

        free = HeapInfo(self.free_segments)
        alloc = HeapInfo(self.alloc_segments)

        print("Heap start {heap.start}\n"
              "Heap end {heap.end}\n"
              "Heap total size {heap.size}\n"
              "Heap allocated {alloc.size}\n"
              "Heap high water mark {heap.high_water_mark}\n"
              "Heap free blocks: {free.size} bytes, {free.blocks} blocks\n"
              "Heap alloc blocks: {alloc.size:d} bytes, {alloc.blocks} blocks\n"
              "Heap largest free block: {free.max}").format(heap=heap, free=free, alloc=alloc)

    def extract_info(self, block):
        if block.corruption_code:
            return
        if block.allocated:
            self.alloc_segments.append(block.size)
        else:
            self.free_segments.append(block.size)

    def extract_file_info(self, block):
        desc = ""
        filename = "UNKNOWN"
        pc = 0xFFFFFFFF
        if block.corruption_code:
            desc = "Block corrupted: {}".format(block.corruption_code)
        else:
            if self.malloc_instrumentation:
                pc = int(block.info["pc"])
                info = gdb_utils.addr2line(pc)
                filename = info.filename
                if not desc:
                    desc = '{info.filename}:{info.line}'.format(info=info)
                gdb.write('.')
                gdb.flush()
            else:
                pc = 0
            if not block.allocated:
                desc = "FREE " + desc
            else:
                desc = "     " + desc
        self.file_size_list.append([pc, block.data, block.size, filename, desc])

HeapStats()


##########################################################################################
class LayerTree(gdb.Command):
    """Print the layer tree of either a given window, or the window currently on screen"""

    def __init__(self):
        super(LayerTree, self).__init__('layer-tree',  gdb.COMMAND_USER)

    def invoke(self, unicode_args, from_tty):
        window_ptr = 0
        if unicode_args:
            argv = gdb.string_to_argv(unicode_args)
            window_ptr = gdb.parse_and_eval("(Window *)" + argv[0])
        else:
            s_modal_window_stacks_name = get_static_variable('s_modal_window_stacks')
            s_modal_window_stacks = gdb.parse_and_eval(s_modal_window_stacks_name)

            window_ptr = 0
            num_stacks = int(gdb.parse_and_eval('NumModalPriorities'))
            window_stack_item_type_ptr = gdb.lookup_type('WindowStackItem').pointer()
            for i in range(num_stacks):
                modal_stack = s_modal_window_stacks[i]
                top_item = modal_stack['window_stack']['list_head']
                if top_item:
                    window_ptr = top_item.cast(window_stack_item_type_ptr)['window']

            if (window_ptr == 0):
                print("Current window is on the app window stack")
                s_app_state = get_static_variable('s_app_state')
                window_ptr = gdb.parse_and_eval("((WindowStackItem *){}.window_stack->list_head)->window".format(s_app_state))
            else:
                print("Current window is a modal")

        window_ptr_type = gdb.lookup_type("Window").pointer()
        # see GDB Bug 10676 https://sourceware.org/bugzilla/show_bug.cgi?id=10676 for why I use str()
        if str(window_ptr.type) != str(window_ptr_type):
            print "Error: argument must be of type {}, this one is {}".format(window_ptr_type, window_ptr.type)

        def print_layer(layer_ptr, level):
            print "{}Layer ({}): frame: ({}, {}, {}, {}), bounds: ({}, {}, {}, {}), hidden: {}, update_proc: {}".format(
                " " * 2 * level,
                layer_ptr,
                layer_ptr["frame"]["origin"]["x"],
                layer_ptr["frame"]["origin"]["y"],
                layer_ptr["frame"]["size"]["w"],
                layer_ptr["frame"]["size"]["h"],
                layer_ptr["bounds"]["origin"]["x"],
                layer_ptr["bounds"]["origin"]["y"],
                layer_ptr["bounds"]["size"]["w"],
                layer_ptr["bounds"]["size"]["h"],
                layer_ptr["hidden"],
                layer_ptr["update_proc"])

        def layer_dump_level(layer_ptr, level):
            def layer_dump_node(layer_ptr, level):
                print_layer(layer_ptr, level)
                if (layer_ptr["first_child"]):
                    layer_dump_level(layer_ptr["first_child"], level + 1)

            while layer_ptr:
                layer_dump_node(layer_ptr, level)
                layer_ptr = layer_ptr["next_sibling"]

        layer = window_ptr["layer"]
        layer_dump_level(layer, 0)

LayerTree()

##########################################################################################
def setup_task_symbols(cmd, args, task_name):
    """ The guts of the WorkerSymbols and AppSymbols commands. Parses the elf file name out of the
    args and sets up the given task (task_name can be 'worker' or 'app')

    """

    if not args:
        cmd.print_usage()
        return

    split_args = args.split(" ", 1)
    if len(split_args) > 1:
        cmd.print_usage()
        return

    elf_file = split_args[0]
    load_addr = gdb_utils.Address(str(gdb.parse_and_eval('&__%s_flash_load_start__' % (task_name))))

    # Get the offsets to the text, data and bss sections
    offsets = {'.bss': 0, '.data': 0}
    for line in sh.arm_none_eabi_objdump('-h', elf_file):
        cols = line.split()
        if len(cols) < 4:
            continue

        if cols[1] in ['.text', '.data', '.bss']:
            offsets[cols[1]] = int(cols[3], 16)

    # Load in the symbols now
    gdb.execute('add-symbol-file %s %d -s .data %d -s .bss %d' % (elf_file, load_addr + offsets['.text'],
                load_addr + offsets['.data'], load_addr + offsets['.bss']))



##########################################################################################
class WorkerSymbols(gdb.Command):
    """Load in symbols for the worker task"""

    def __init__(self):
        super(WorkerSymbols, self).__init__('worker_symbols', gdb.COMMAND_USER)

    def print_usage(self):
        print "Load in symbols for the worker task\n" \
              "Expected command format:\n" \
              "worker_symbols <worker-elf-file>"

    def complete(self, text, word):
        return gdb.COMPLETE_FILENAME

    def invoke(self, unicode_args, from_tty):
        setup_task_symbols(self, unicode_args, 'worker')
        return

WorkerSymbols()


##########################################################################################
class AppSymbols(gdb.Command):
    """Load in symbols for the app task"""

    def __init__(self):
        super(AppSymbols, self).__init__('app_symbols', gdb.COMMAND_USER)

    def print_usage(self):
        print "Load in symbols for the app task\n" \
              "Expected command format:\n" \
              "app_symbols <app-elf-file>"

    def complete(self, text, word):
        return gdb.COMPLETE_FILENAME

    def invoke(self, unicode_args, from_tty):
        setup_task_symbols(self, unicode_args, 'app')
        return

AppSymbols()

##########################################################################################
class RebootReason(gdb.Command):
    """Print RebootReason stored in the RTC registers"""

    def __init__(self):
        super(RebootReason, self).__init__('reboot_reason', gdb.COMMAND_USER)

    def print_usage(self):
        print "Print Reboot reason from RTC registers.\n"

    def invoke(self, unicode_args, from_tty):
        gdb.execute('p (RebootReasonCode)(*(uint8_t *)0x40002864)')
        gdb.execute('p/x *((RebootReason *)0x40002864)')

RebootReason()

class SharedCircularBuffer(object):

    def __init__(self, circular_buffer, client):
        if not isinstance(circular_buffer, gdb.Value):
            circular_buffer = gdb.parse_and_eval(circular_buffer)
        if not isinstance(client, gdb.Value):
            client = gdb.parse_and_eval(client)

        if hasattr(circular_buffer.type, 'name'):
            # The .name attribute is only available in the 4.9 and later versions
            # of the arm toolchain.
            if circular_buffer.type.name != 'SharedCircularBuffer':
                raise ValueError('circular_buffer is of type %s, not '
                                 'SharedCircularBuffer.' %
                                        circular_buffer.type.name)
            if client.type.name != 'SharedCircularBufferClient':
                raise ValueError('client is of type %s, not '
                                 'SharedCircularBufferClient.' %
                                        client.type.name)

        self.buffer = circular_buffer['buffer']
        self.size = int(circular_buffer['buffer_size'])
        self.write_index = int(circular_buffer['write_index'])
        self.read_index = int(client['read_index'])

    def read_and_consume(self, size):
        data = []
        for _ in xrange(size):
            if self.read_index == self.write_index:
                break
            data.append(chr(self.buffer[self.read_index]))
            self.read_index = (self.read_index + 1) % self.size
        return ''.join(data)

class DumpLogBuffer(gdb.Command):
    "Dump the messages in the log buffer which haven't been flushed to flash."

    LogMessageStruct = struct.Struct('>IBBH16s')  # Yep, big-endian.
    LogMessage = namedtuple('LogMessage',
                            'timestamp log_level message_length '
                            'line_number filename')

    def __init__(self):
        super(DumpLogBuffer, self).__init__(
                'pbl log-buffer', gdb.COMMAND_USER, gdb.COMPLETE_NONE)

    @staticmethod
    def _get_build_id():
        # For some reason GDB sometimes has trouble figuring out the type of
        # TINTIN_BUILD_ID. Help it along with an explicit cast.
        try:
            # Recent firmwares (as of 144bf34) have an anonymous struct which is
            # typedef'ed to ElfExternalNote.
            id_struct_type = gdb.lookup_type('ElfExternalNote')
        except gdb.error:
            # Older firmwares have a struct ElfExternalNote which is not
            # typedef'ed.
            id_struct_type = gdb.lookup_type('struct ElfExternalNote')
        id_struct = gdb.parse_and_eval('TINTIN_BUILD_ID').cast(id_struct_type)
        length = int(id_struct['data_length'])
        offset = int(id_struct['name_length'])
        return ''.join('%02x' % int(id_struct['data'][x])
                       for x in xrange(offset, offset+length))

    def invoke(self, arg, from_tty):
        gdb.write('Build ID: %s\n' % self._get_build_id())
        s_buffer = get_static_variable('s_buffer', _file='advanced_logging.c')
        s_buffer_client = get_static_variable('s_buffer_client', _file='advanced_logging.c')
        buf = gdb.parse_and_eval(s_buffer)
        client = gdb.parse_and_eval(s_buffer_client)
        reader = SharedCircularBuffer(buf, client)
        while True:
            length_char = reader.read_and_consume(1)
            if not length_char:
                break
            length = ord(length_char)
            length -= self.LogMessageStruct.size
            assert length >= 0
            header_bytes = reader.read_and_consume(self.LogMessageStruct.size)
            header = self.LogMessage._make(
                    self.LogMessageStruct.unpack(header_bytes))
            message = reader.read_and_consume(length)
            gdb.write('{ts:%Y-%m-%d %H:%M:%S}:000GMT {file}:{line} {msg}\n'
                    .format(
                        ts=datetime.datetime.utcfromtimestamp(header.timestamp),
                        file=ctypes.c_char_p(header.filename).value,
                        line=header.line_number,
                        msg=ctypes.c_char_p(message).value))

DumpLogBuffer()

##########################################################################################
class DumpNotificationsApp(gdb.Command):
    """Dump the list of notifications from the Notifications App"""

    def __init__(self):
        super(DumpNotificationsApp, self).__init__('pbl notif_app',
                                                   gdb.COMMAND_USER,
                                                   gdb.COMPLETE_NONE)

    def print_usage(self):
        print "Attempts to dump the notifications that were loaded" \
              " in the Notifications app"

    def invoke(self, unicode_args, from_tty):
        symstr = "'src/fw/apps/system_apps/notifications_app.c'::s_data"
        app_data =  gdb.parse_and_eval(symstr)
        if app_data == 0:
            print "Notifications app was not open?"
            return

        # For some reason, I couldn't get a pretty printer
        # to work for the Uuid struct type...
        def string_from_uuid(u):
            uint8_ptr_type = gdb.lookup_type("uint8_t").array(16)
            uuid_bytes = u.cast(uint8_ptr_type)
            s = ""
            for n in xrange(16):
                s += "%02x" % int(uuid_bytes[n])
            return s

        uuid_set = set()

        print "Notification list:"
        print "-----"
        node = app_data["notification_list"]
        node_type = node.type
        while node:
            u = node["id"]
            uuid_str = string_from_uuid(u)
            print "UUID: %s" % uuid_str
            if uuid_str in uuid_set:
                print "WARNING: Dupe UUID %s" % uuid_str
            else:
                uuid_set.add(uuid_str)
            node = node["node"]["next"].cast(node_type)


        print "\nLoaded notification list:"
        print "-----"
        node = app_data["loaded_notification_list"]
        node_type = node.type
        while node:
            u = node["notification"]["header"]["id"]
            ts = node["notification"]["header"]["timestamp"]
            ancs_uid = node["notification"]["header"]["ancs_uid"]
            print "UUID: %s TS: %u ANCS_UID: %u" % (string_from_uuid(u), ts,
                                                    ancs_uid)

            attr_list = node["notification"]["attr_list"]
            for n in xrange(attr_list["num_attributes"]):
                attr_ptr = attr_list["attributes"][n]
                identifier = int(attr_ptr["id"])
                if identifier == 1 or identifier == 2 or identifier == 3:
                    print attr_ptr["cstring"]
            print ""
            node = node["node"]["next"].cast(node_type)

DumpNotificationsApp()


class _rangeInfo(collections.namedtuple('_rangeInfo', 'start end name')):
    __slots__ = ()

    @classmethod
    def parse_line(cls, line):
        range_match = re.search(r'(0x\w+)\s+\-\s+(0x\w+) is (.+)$', line)

        if range_match:
            start = int(range_match.group(1), 16)
            end = int(range_match.group(2), 16)
            name = range_match.group(3)
            return cls(start, end, name)

    @property
    def size(self):
        return self.end - self.start

    def __str__(self):
        return ('0x{self.start:08x} - 0x{self.end:08x} = '
                '{self.size:8d} : {self.name}'.format(self=self))


class FilesInfo(gdb.Command):
    """gdb 'info files' command in sorted order with sizes"""

    def __init__(self):
        super(FilesInfo, self).__init__('pbl info-files', gdb.COMMAND_USER, gdb.COMPLETE_SYMBOL)

    def invoke(self, unicode_args, from_tty):
        info_data = gdb.execute("info files", from_tty, True)

        info_data_lines = (line for line in info_data.splitlines() if line)
        memory_ranges = []

        for line in info_data_lines:
            if not line.startswith('\t0x'):
                print line
            else:
                memory_ranges.append(_rangeInfo.parse_line(line))

        memory_ranges.sort()

        for memory_range in memory_ranges:
            print ("\t%s" % str(memory_range))

FilesInfo()

class PNGDump(gdb.Command):
    """Dump a GBitmap or framebuffer to a png file."""

    sys.path.append(os.path.abspath('.'))

    def __init__(self):
        super(PNGDump, self).__init__('pbl to_png', gdb.COMMAND_USER, gdb.COMPLETE_SYMBOL)

    def print_usage(self):
        print "Dump a GBitmap or framebuffer to a png file."
        print "pbl to_png <gbitmap | framebuffer> [filename]"

    def invoke(self, unicode_args, from_tty):
        from tools.pbi2png import pbi_struct, pbi_to_png, \
                                  pbi_format, pbi_is_palettized, palette_size
        if not unicode_args:
            print "non unicode"
            self.print_usage()
            return

        split_args = unicode_args.split()

        if len(split_args) not in (1, 2):
            self.print_usage()
            return

        # TODO: Use variable name and sequential numbering , eg. src_bitmap.1.png
        filename = split_args[1] if (len(split_args) == 2) else "out.png"

        source_value = gdb.parse_and_eval(split_args[0])
        gbitmap_type = gdb.lookup_type("struct GBitmap")

        source_type = source_value.type
        source_type = source_type.strip_typedefs()

        pbi = pbi_struct()
        pixel_ptr = None
        if source_type == gdb.lookup_type("FrameBuffer"):
            # This is a framebuffer, construct a GBitmap around it.
            # See framebuffer_get_as_bitmap() for more information.
            bytes_per_row = gdb.lookup_global_symbol("FrameBuffer_BytesPerRow")
            pbi = pbi_struct(bytes_per_row.value(),
                             4098,  # Info flags, hardcoded for 8-bit
                             0, 0,
                             gdb.lookup_global_symbol("FrameBuffer_MaxX").value(),
                             gdb.lookup_global_symbol("FrameBuffer_MaxY").value())
            pixel_ptr = source_value["buffer"]
        else:  # FIXME: For now, just assume this is a GBitmap
            pbi = pbi_struct(source_value["row_size_bytes"],
                             source_value["info_flags"],
                             source_value["bounds"]["origin"]["x"],
                             source_value["bounds"]["origin"]["y"],
                             source_value["bounds"]["size"]["w"],
                             source_value["bounds"]["size"]["h"])
            pixel_ptr = source_value["addr"]

        size = pbi.bounds_h * pbi.stride

        uint8_t = gdb.lookup_type("uint8_t")
        pix_arr = pixel_ptr.cast(uint8_t.pointer())
        pixels = bytearray([int((pix_arr + x).dereference()) for x in xrange(size)])

        pbi_fmt = pbi_format(pbi.info)
        if pbi_is_palettized(pbi_fmt):
            # palette gets appended to end of pixel buffer
            sz = palette_size(pbi_fmt)
            palette_arr = source_value["palette"].cast(uint8_t.pointer())
            pixels.extend([int((palette_arr + x).dereference()) for x in xrange(sz)])

        png = pbi_to_png(pbi, pixels)
        if png:
            png.save(filename)
            print "Saved to {}".format(filename)

PNGDump()


class FaultWizard(gdb.Command):
    """Finds and sets the register values (lr,pc,sp) for a fault"""

    def __init__(self):
        super(FaultWizard, self).__init__('pbl fault_wizard', gdb.COMMAND_USER, gdb.COMPLETE_SYMBOL)

    def print_usage(self):
        print "Set the values from the previous crash into the " \
              "current thread's real sp,lr,pc registers"

    def get_and_set_reg(self, reg_name):
        reg_var = get_static_variable("s_fault_saved_%s" % reg_name)
        reg_val = gdb.parse_and_eval(reg_var)

        # Check if the register is set
        if int(reg_val) == 0:
            print "Register %s was zero. Did not set it" % reg_name
            return

        # Set the frame's register
        gdb.execute("set $%s=%s" % (reg_name, reg_val))
        print "Set register %s to 0x%x" % (reg_name, reg_val)

    def invoke(self, unicode_args, from_tty):
        self.get_and_set_reg("sp")
        self.get_and_set_reg("lr")
        self.get_and_set_reg("pc")


FaultWizard()


class PrintMetadata(gdb.Command):
    """Prints information about the build running on the watch"""

    def __init__(self):
        super(PrintMetadata, self).__init__('pbl metadata', gdb.COMMAND_USER, gdb.COMPLETE_SYMBOL)

    def print_usage(self):
        print "Prints metadata from TINTIN_METADATA in a readable format"

    def invoke(self, unicode_args, from_tty):
        metadata = TintinMetadata()
        print(metadata)

PrintMetadata()


if __name__ == '__main__':
    import logging
    logging.basicConfig(level=logging.INFO)
