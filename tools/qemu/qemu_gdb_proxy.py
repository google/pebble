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
GDB server proxy for the QEMU emulator running a Pebble machine.

This proxy sits between gdb and the gdb server implemented in QEMU. Its primary purpose is to
implement support for the "info threads" and related gdb commands. The QEMU gdb server is not thread
aware and doesn't have any FreeRTOS knowledge such that it can figure out the FreeRTOS threads
created in the Pebble. 

This proxy talks to the QEMU gdb server using primitive gdb remote commands and inspects the 
FreeRTOS task structures to figure out which threads have been created, their saved registers, etc. 
and then returns that information to gdb when it asks for thread info from the target system. For 
most other requests received from gdb, this proxy simply acts as a passive pass thru to the QEMU gdb
server.

This module is designed to be run as a separate process from both QEMU and gdb. It connects to the
gdb socket created by QEMU and accepts connections from gdb. The intent is that this module would 
be launched whenever QEMU is launched and likewise taken down whenever QEMU exits. To support this, 
we exit this process whenever we detect that the QEMU gdb server connection has closed.
"""

import logging, socket
from struct import unpack
from time import sleep
import sys
import time
import argparse
import select


CTRL_C_CHARACTER = '\3'

##########################################################################################
class QemuGdbError(Exception):
    pass


##########################################################################################
def byte_swap_uint32(val):
    """ Return a byte-swapped 32-bit value """
    return  (   ((val & 0xFF000000) >> 24)
              | ((val & 0x00FF0000) >> 8)
              | ((val & 0x0000FF00) << 8)
              | ((val & 0x000000FF) << 24))


##########################################################################################
class PebbleThread(object):
    """ This class encapsulates the information about a thread on the Pebble """

    # Mapping of register name to register index
    reg_name_to_index = {name: num for num, name in enumerate(
          'r0 r1 r2 r3 r4 r5 r6 r7 r8 r9 r10 r11 r12 sp lr pc xpsr'.split())}

    # Offset of each register on the thread's stack
    # stack_offset -> register_index
    stack_offset_to_reg_index_v2 = [        # Used in Snowy, Cortex-M4
            (0x28, 0),    # r0
            (0x2C, 1),    # r1
            (0x30, 2),    # r2
            (0x34, 3),    # r3
            (0x04, 4),    # r4
            (0x08, 5),    # r5
            (0x0C, 6),    # r6
            (0x10, 7),    # r7
            (0x14, 8),    # r8
            (0x18, 9),    # r9
            (0x1C, 10),   # r10
            (0x20, 11),   # r11
            (0x38, 12),   # r12
            (0x3C, 14),   # lr
            (0x40, 15),   # pc
            (0x44, 16),   # xpsr
        ]
    thread_state_size_v2 = 0x48

    stack_offset_to_reg_index_v1 = [      # Used in Tintin, Cortex-M3
            (0x24, 0),    # r0
            (0x28, 1),    # r1
            (0x2C, 2),    # r2
            (0x30, 3),    # r3
            (0x04, 4),    # r4
            (0x08, 5),    # r5
            (0x0C, 6),    # r6
            (0x10, 7),    # r7
            (0x14, 8),    # r8
            (0x18, 9),    # r9
            (0x1C, 10),   # r10
            (0x20, 11),   # r11
            (0x34, 12),   # r12
            (0x38, 14),   # lr
            (0x3C, 15),   # pc
            (0x40, 16),   # xpsr
        ]
    thread_state_size_v1 = 0x44


    def __init__(self, id, ptr, running, name, registers):
        self.id = id
        self.ptr = ptr
        self.running = running
        self.name = name
        self.registers = registers


    def set_register(self, reg_index, value):
        self.registers[reg_index] = value


    def get_register(self, reg_index):
        return self.registers[reg_index]


    def __repr__(self):
        return "<Thread id:%d, ptr:0x%08X, running:%r, name:%s, registers:%r" % (self.id, self.ptr,
                  self.running, self.name, self.registers)


##########################################################################################
class QemuGdbProxy(object):
    """
    This class implements a GDB server listening for a gdb connection on a specific port.
    It connects to and acts as a proxy to yet another gdb server running on the target system. 
    This proxy implements advanced gdb commands like getting thread info by looking up the values of
    specific FreeRTOS symbols and querying the FreeRTOS data structures on the target system.
    """

    ##########################################################################################
    def __init__(self, port, target_host, target_port, connect_timeout):
        # The "target" is the remote system we are debugging. The target implements a basic
        #  gdb remote server
        self.target_host = target_host
        self.target_port = target_port
        self.target_socket = None
        self.connect_timeout = connect_timeout

        # The "client" is gdb
        self.client_accept_socket = None
        self.client_accept_port = port
        self.client_conn_socket = None

        self.packet_size = 2048
        self.active_thread_id = 0       # Selected by GDB
        self.threads = {}               # key is the thread id, value is a PebbleThread object

        # The QEMU gdb remote server always assigns a thread ID of 1 to it's one and only thread
        self.QEMU_MONITOR_CURRENT_THREAD_ID = 1

        # Free RTOS symbols we need to look up in order to inspect FreeRTOS threads
        symbol_list = [
            "uxFreeRTOSRegisterStackingVersion",
            "pxCurrentTCB",
            "pxReadyTasksLists",
            "xDelayedTaskList1",
            "xDelayedTaskList2",
            "pxDelayedTaskList",
            "pxOverflowDelayedTaskList",
            "xPendingReadyList",
            "xTasksWaitingTermination",
            "xSuspendedTaskList",
            "uxCurrentNumberOfTasks",
        ]
        self.symbol_dict = {symbol: None for symbol in symbol_list}

        self.got_all_symbols = False


    ##########################################################################################
    def _fetch_socket_data(self, timeout=None):
        """ Fetch available data from our sockets (client and target). Block until any
        data is available, or until the target connection is closed. If we detect that the
        target connection has closed, we exit this app.
        
        If we detect that the client connection (from gdb) has closed, we wait for a new connection
        request from gdb. 
        
        retval:
          (target_data, client_data)

        """

        target_data = ''
        client_data = ''

        while (not target_data and not client_data):
            # Form our read list. The target socket is always in the read list. Depending on if we
            #  are waiting for a client connection or not, we either put the client_accept_socket or
            # client_conn_socket in the list.
            if self.client_conn_socket is not None:
                read_list = [self.target_socket, self.client_conn_socket]
            else:
                read_list = [self.target_socket, self.client_accept_socket]

            readable, writable, errored = select.select(read_list, [], [], timeout)
            # If nothing ready, we must have timed out
            if not readable:
                logging.debug("read timeout")
                break

            # Data available from target?
            if self.target_socket in readable:
                target_data = self.target_socket.recv(self.packet_size)
                if not target_data:
                    raise QemuGdbError("target system disconnected")
                logging.debug("got target data: '%s' (0x%s) " % (target_data,
                                                                target_data.encode('hex')))

            # Data available from client?
            if self.client_conn_socket is not None:
                if self.client_conn_socket in readable:
                    client_data = self.client_conn_socket.recv(self.packet_size)
                    if not client_data:
                        logging.info("client connection closed")
                        self.client_conn_socket.close()
                        self.client_conn_socket = None
                    logging.debug("got client data: '%s' (0x%s) " % (client_data,
                                                                     client_data.encode('hex')))

            # Connection request from client?
            else:
                if self.client_accept_socket in readable:
                    self.client_conn_socket, _ = self.client_accept_socket.accept()
                    logging.info("Connected to client")

        return (target_data, client_data)


    ##########################################################################################
    def _create_packet(self, data):
        checksum = sum(ord(c) for c in data) % 256
        packet = "$%s#%02X" % (data, checksum)

        logging.debug('--<<<<<<<<<<<< GDB packet: %s', packet)
        return packet


    ##########################################################################################
    def _target_read_register(self, reg_index):
        """ Fetch the value of the given register index from the active thread """
        try:
            thread = self.threads[self.active_thread_id]
        except KeyError:
            raise QemuGdbError("Unknown thread id")
        return thread.get_register(reg_index)


    ##########################################################################################
    def _target_write_register(self, reg_index, value):
        """ Update the value of the given register index in the active thread """
        try:
            thread = self.threads[self.active_thread_id]
        except KeyError:
            raise QemuGdbError("Unknown thread id")
        print "TODO: NEED TO WRITE TO THREAD STACK ON TARGET TOO"
        thread.set_register(reg_index, value)


    ##########################################################################################
    def _target_read_memory(self, address, bytes):
        request = self._create_packet('m %08X,%08X' % (address, bytes))
        self.target_socket.send(request)

        # read response
        data = ''
        while True:
            target_data = self.target_socket.recv(self.packet_size)
            if not target_data:
                raise QemuGdbError("target system disconnected")
            data += target_data
            if "$" in data and "#" in data:
                break

        _, data = data.split('$', 1)
        logging.debug("Received target response: %s" % (data))

        resp = data.split('#', 1)[0]
        if resp.startswith('E '):
            raise QemuGdbError("Error response %s", resp)
        return resp


    ##########################################################################################
    def _target_read_uint32(self, address):
        hex = self._target_read_memory(address, 4)
        value = int(hex, 16)
        return byte_swap_uint32(value)


    ##########################################################################################
    def _target_read_uint8(self, address):
        hex = self._target_read_memory(address, 1)
        value = int(hex, 16)
        return value


    ##########################################################################################
    def _target_read_cstr(self, address, max_len):
        str_hex = self._target_read_memory(address, max_len)
        str = str_hex.decode('hex')
        return str.split('\0', 1)[0]


    ##########################################################################################
    def _target_collect_thread_info(self):

        # FreeRTOS params used to collect thread info
        FRTOS_LIST_NEXT_OFFSET = 16
        FRTOS_LIST_WIDTH = 20
        FRTOS_LIST_ELEM_NEXT_OFFSET = 8
        FRTOS_LIST_ELEM_CONTENT_OFFSET = 12
        FRTOS_THREAD_STACK_OFFSET = 0
        FRTOS_THREAD_NAME_OFFSET = 84
        FRTOS_MAX_PRIORITIES = 5

        num_threads = self._target_read_uint32(self.symbol_dict['uxCurrentNumberOfTasks'])

        # Figure out the register stacking
        if self.symbol_dict['uxFreeRTOSRegisterStackingVersion']:
            reg_stacking_version = self._target_read_uint8(
                                    self.symbol_dict['uxFreeRTOSRegisterStackingVersion'])
        else:
            reg_stacking_version = 1

        if reg_stacking_version == 1:
              stack_offset_to_reg_index = PebbleThread.stack_offset_to_reg_index_v1
              thread_state_size = PebbleThread.thread_state_size_v1
        elif reg_stacking_version == 2:
              stack_offset_to_reg_index = PebbleThread.stack_offset_to_reg_index_v2
              thread_state_size = PebbleThread.thread_state_size_v2
        else:
              raise QemuGdbError("Unsupported uxFreeRTOSRegisterStackingVersion of %d" %
                                    reg_stacking_version)


        # Get total number of threads and current thread ID
        num_threads = self._target_read_uint32(self.symbol_dict['uxCurrentNumberOfTasks'])
        current_thread = self._target_read_uint32(self.symbol_dict['pxCurrentTCB'])
        self.threads = {}

        # Get the address of each list
        list_addresses = []
        address = self.symbol_dict['pxReadyTasksLists']
        for i in range(FRTOS_MAX_PRIORITIES):
            list_addresses.append(address + i * FRTOS_LIST_WIDTH)

        for name in ['xDelayedTaskList1', 'xDelayedTaskList2', 'xPendingReadyList',
                     'xSuspendedTaskList', 'xTasksWaitingTermination']:
            list_addresses.append(self.symbol_dict[name])


        # Fetch the tasks from each list
        for list in list_addresses:
            thread_count = self._target_read_uint32(list)
            if thread_count == 0:
                continue

            # Location of first item
            elem_ptr = self._target_read_uint32(list + FRTOS_LIST_NEXT_OFFSET)

            # Loop through the list
            prev_elem_ptr = -1
            while (thread_count > 0 and elem_ptr != 0 and elem_ptr != prev_elem_ptr
                    and len(self.threads) < num_threads):

                thread_ptr = self._target_read_uint32(elem_ptr + FRTOS_LIST_ELEM_CONTENT_OFFSET)
                thread_running = (thread_ptr == current_thread)

                # The QEMU gdb server assigns the active thread a thread ID of 1 and if we change it
                #  to something else (like the TCB ptr), then things are not ideal. For example, gdb
                #  will display a "The current thread <Thread ID 1> has terminated" message.
                # So, we will preserve 1 for the current thread and assign the TCB ptr for the
                #  others
                if thread_running:
                    thread_id = self.QEMU_MONITOR_CURRENT_THREAD_ID
                else:
                    thread_id = thread_ptr
                thread_name = self._target_read_cstr(thread_ptr + FRTOS_THREAD_NAME_OFFSET, 32)

                stack = self._target_read_uint32(thread_ptr + FRTOS_THREAD_STACK_OFFSET)
                registers = [0] * len(PebbleThread.reg_name_to_index)
                for (offset, reg_index) in stack_offset_to_reg_index:
                    registers[reg_index] = self._target_read_uint32(stack + offset)
                registers[13] = stack + thread_state_size

                # Create the thread instance
                thread = PebbleThread(id=thread_id, ptr=thread_ptr, running=thread_running,
                            name=thread_name, registers=registers)
                self.threads[thread_id] = thread
                logging.debug("Got thread info: %r" % (thread))

                # Another thread in this list?
                prev_elem_ptr = elem_ptr
                elem_ptr = self._target_read_uint32(elem_ptr + FRTOS_LIST_ELEM_NEXT_OFFSET)
                thread_count -= 1


    ##########################################################################################
    def _handle_set_active_thread_req(self, data):
        num = int(data, 16)
        if (num == -1):   # All threads
            return
        elif (num == 0):  # Any thread
            num = self.QEMU_MONITOR_CURRENT_THREAD_ID
        self.active_thread_id = num
        return self._create_packet("OK")


    ##########################################################################################
    def _handle_continue_req(self, msg):
        """ The format of this is: 'vCont[;action[:thread-id]]...'
        The QEMU gdb server only understands a thread id of 1, so if we pass it other thread ids,
        it will barf. 
        """
        if ';' not in msg:
            return None
        action_thread_pair = msg.split(';')[1]
        if ':' in action_thread_pair:
            action = action_thread_pair.split(':')[0]
        else:
            action = action_thread_pair

        # Send to target with the thread ID
        packet = self._create_packet("vCont;%s" % (action))
        self.target_socket.send(packet)

        # Change back to active thread of 1
        self.active_thread_id = self.QEMU_MONITOR_CURRENT_THREAD_ID
        return ''


    ##########################################################################################
    def _handle_thread_is_alive_req(self, data):
        num = int(data, 16)
        if (num == -1 or num == 0):   # All threads
            return self._create_packet("OK")

        if num in self.threads:
            return self._create_packet("OK")
        return self._create_packet("E22")


    ##########################################################################################
    def _handle_get_all_registers_req(self):
        """ Get all registers for the active thread """

        resp = ''
        for i in range(len(PebbleThread.reg_name_to_index)):
            value = self._target_read_register(i)
            resp += "%08X" % (byte_swap_uint32(value))
        return self._create_packet(resp)



    ##########################################################################################
    def _handle_query_req(self, msg):
        msg = msg.split('#')[0]
        query = msg.split(':')
        logging.debug('GDB received query: %s', query)

        if query is None:
            logging.error('GDB received query packet malformed')
            return None

        elif query[0] == 'C':
            return self._create_packet("%d" % (self.active_thread_id))
        
        elif query[0] == 'fThreadInfo':
            if not self.got_all_symbols:
                # NOTE: When running the 4.9 gcc tool chain, gdb asks for thread info right
                # after attaching, before we have a chance to look up symbols, so respond
                # with "last thread" if we don't have symbols yet.
                return self._create_packet("l")        # last
            self._target_collect_thread_info()
            # For some strange reason, if the active thread is first, the first "i thread" gdb
            # command only displays that one thread, so reverse sort to put it at the end
            id_strs = ("%016x" % id for id in sorted(self.threads.keys(), reverse=True))
            return self._create_packet("m" + ",".join(id_strs))
        
        elif query[0] == 'sThreadInfo':
            return self._create_packet("l")        # last

        elif query[0].startswith('ThreadExtraInfo'):
            id_str = query[0].split(',')[1]
            id = int(id_str, 16)

            found_thread = self.threads.get(id, None)
            if found_thread is None:
                resp = "<INVALID THREAD ID: %d>" % (id)
            elif found_thread.running:
                resp = "%s 0x%08X: Running" % (found_thread.name, found_thread.ptr)
            else:
                resp = "%s 0x%08X" % (found_thread.name, found_thread.ptr)
            return self._create_packet(resp.encode('hex'))

        elif 'Symbol' in query[0]:
            if query[2] != '':
                sym_name = query[2].decode('hex')
                if query[1] != '':
                    sym_value = int(query[1], 16)
                    logging.debug("Setting value of symbol '%s' to 0x%08x" % (sym_name, sym_value))
                    self.symbol_dict[sym_name] = sym_value
                else:
                    logging.debug("Could not find value of symbol '%s'" % (sym_name))
                    self.symbol_dict[sym_name] = ''


            # Anymore we need to look up?
            symbol = None
            for x, y in self.symbol_dict.items():
                if y is None:
                    symbol = x
                    break
            if symbol is not None:
                logging.debug("Asking gdb to lookup symbol %s" % (symbol))
                return self._create_packet('qSymbol:%s' % (symbol.encode('hex')))
            else:
                self.got_all_symbols = True
                return self._create_packet('OK')
        
        else:
            return None


    ##########################################################################################
    def _handle_request(self, msg):
        """ See if we want to handle a request directly here in the proxy 
        
        retval: resp,
          resp: Response to return. 
                if None, proxy doesn't deal with the request directly
        """
        
        logging.debug('-->>>>>>>>>>>> GDB req packet: %s', msg)

        msg = msg.split('#')[0]

        # query command
        if msg[1] == 'q':
            return self._handle_query_req(msg[2:])
            
        elif msg[1] == 'H':
            if msg[2] == 'c':
                return None
            else:
                return self._handle_set_active_thread_req(msg[3:])

        elif msg[1] == 'T':
            return self._handle_thread_is_alive_req(msg[2:])

        elif msg[1] == 'g':
            if (self.active_thread_id <= 0
                      or self.active_thread_id == self.QEMU_MONITOR_CURRENT_THREAD_ID):
                return None
            else:
                return self._handle_get_all_registers_req()

        elif msg[1] == 'p':
            # 'p <n>' : read value of register n
            if self.active_thread_id == self.QEMU_MONITOR_CURRENT_THREAD_ID:
                return None
            else:
                msg = msg[2:]
                reg_num = int(msg, 16)
                value = self._target_read_register(reg_num)
                return self._create_packet("%08X" % (byte_swap_uint32(value)))

        elif msg[1] == 'P':
            # 'P <n>=<r>' : set value of register n to r
            if self.active_thread_id == self.QEMU_MONITOR_CURRENT_THREAD_ID:
                return None
            else:
                msg = msg[2:].split('=')
                reg_num = int(msg[0], 16)
                val = int(msg[1], 16)
                val = byte_swap_uint32(val)
                self._target_write_register(reg_num, val)
                return self._create_packet("OK")

        elif msg[1:].startswith('vCont'):
            return self._handle_continue_req(msg[1:])

        else:
            return None


    ##########################################################################################
    def run(self):
        """ Run the proxy """

        # Connect to the target system first
        logging.info("Connecting to target system on %s:%s" % (self.target_host, self.target_port))

        start_time = time.time()
        connected = False
        self.target_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        while not connected and (time.time() - start_time < self.connect_timeout):
            try:
                self.target_socket.connect((self.target_host, self.target_port))
                connected = True
            except socket.error:
                self.target_socket.close()
                self.target_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                time.sleep(0.1)

        if not connected:
            raise QemuGdbError("Unable to connect to target system on %s:%s. Is the emulator"
                " running?" % (self.target_host, self.target_port))

        logging.info("Connected to target system on %s:%s" % (self.target_host,
                        self.target_port))

        # Open up our socket to accept connect requests from the client (gdb)
        self.client_accept_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.client_accept_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.client_accept_socket.bind(('', self.client_accept_port))
        self.client_accept_socket.listen(5)

        # Empty out any unsolicited data sent from the target
        (target_data, client_data) = self._fetch_socket_data(timeout=0.1)


        # --------------------------------------------------------------------------------------
        # Loop processing requests
        data = ''
        while True:
            # read more data from client until we get at least one packet
            while True:
                (target_data, client_data) = self._fetch_socket_data()

                # Pass through any response from the target back to gdb
                if target_data and self.client_conn_socket is not None:
                    self.client_conn_socket.send(target_data)

                # Ctrl-C interrupt?
                if CTRL_C_CHARACTER in client_data:
                    self.target_socket.send(CTRL_C_CHARACTER)
                    client_data = client_data[client_data.index(CTRL_C_CHARACTER)+1:]

                data += client_data
                if "$" in data and "#" in data:
                    break

            # Process all complete packets we have received from the client
            while "$" in data and "#" in data:
                data = data[data.index("$"):]
                logging.debug("Processing remaining data: %s" % (data))
                end = data.index("#") + 3  # 2 bytes of checksum
                packet = data[0:end]
                data = data[end:]

                # decode and prepare resp
                logging.debug("Processing packet: %s" % (packet))
                resp = self._handle_request(packet)

                # If it's nothing we care about, pass to target and return the response back to
                #  client
                if resp is None:
                    logging.debug("Sending request to target: %s" % (packet))
                    self.target_socket.send(packet)

                # else, we generated our own response that needs to go to the client
                elif resp != '':
                    self.client_conn_socket.send('+' + resp)

                    # wait for ack from the client
                    (target_data, client_data) = self._fetch_socket_data()
                    if target_data:
                        self.client_conn_socket.send(target_data)

                    if client_data[0] != '+':
                        logging.debug('gdb client did not ack')
                    else:
                        logging.debug('gdb client acked')

                    # Add to our accumulated content
                    data += client_data[1:]


####################################################################################################
if __name__ == '__main__':
    # Collect our command line arguments
    parser = argparse.ArgumentParser(formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument('--port', type=int, default=1233,
            help="Port to accept incoming connections on")
    parser.add_argument('--target', default='localhost:1234',
            help="target to connect to ")
    parser.add_argument('--connect_timeout', type=float, default=1.0,
            help="give up if we can't connect to the target within this timeout (sec)")
    parser.add_argument('--debug', action='store_true',
            help="Turn on debug logging")
    args = parser.parse_args()

    level = logging.INFO
    if args.debug:
      level = logging.DEBUG
    logging.basicConfig(level=level)

    (target_host, target_port) = args.target.split(':')
    proxy = QemuGdbProxy(port=args.port, target_host=target_host, target_port=int(target_port),
              connect_timeout=args.connect_timeout)
    proxy.run()

