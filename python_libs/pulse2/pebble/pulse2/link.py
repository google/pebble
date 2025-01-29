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

from __future__ import absolute_import

import logging
import threading

import serial

from . import exceptions, framing, ppp, transports
from . import logging as pulse2_logging
from . import pcap_file

try:
    import pyftdi.serialext
except ImportError:
    pass

logger = logging.getLogger(__name__)
logger.addHandler(logging.NullHandler())

DBGSERIAL_PORT_SETTINGS = dict(baudrate=1000000, timeout=0.1,
                               interCharTimeout=0.0001)


def get_dbgserial_tty():
    # Local import so that we only depend on this package if we're attempting
    # to autodetect the TTY. This package isn't always available (e.g., MFG),
    # so we don't want it to be required.
    try:
        import pebble_tty
        return pebble_tty.find_dbgserial_tty()
    except ImportError:
        raise exceptions.TTYAutodetectionUnavailable


class Interface(object):
    '''The PULSEv2 lower data-link layer.

    An Interface object is roughly analogous to a network interface,
    somewhat like an Ethernet port. It provides connectionless service
    with PULSEv2 framing, which upper layers build upon to provide
    connection-oriented service.

    An Interface is bound to an I/O stream, such as a Serial port, and
    remains open until either the Interface is explicitly closed or the
    underlying I/O stream is closed from underneath it.
    '''

    def __init__(self, iostream, capture_stream=None):
        self.logger = pulse2_logging.TaggedAdapter(
                logger, {'tag': type(self).__name__})
        self.iostream = iostream
        self.closed = False
        self.close_lock = threading.RLock()
        self.default_packet_handler_cb = None
        self.sockets = {}

        self.pcap = None
        if capture_stream:
            self.pcap = pcap_file.PcapWriter(
                    capture_stream, pcap_file.LINKTYPE_PPP_WITH_DIR)

        self.receive_thread = threading.Thread(target=self.receive_loop)
        self.receive_thread.daemon = True
        self.receive_thread.start()

        self.simplex_transport = transports.SimplexTransport(self)

        self._link = None
        self.link_available = threading.Event()
        self.lcp = ppp.LinkControlProtocol(self)
        self.lcp.on_link_up = self.on_link_up
        self.lcp.on_link_down = self.on_link_down
        self.lcp.up()
        self.lcp.open()

    @classmethod
    def open_dbgserial(cls, url=None, capture_stream=None):
        if url is None:
            url = get_dbgserial_tty()
        elif url == 'qemu':
            url = 'socket://localhost:12345'
        ser = serial.serial_for_url(url, **DBGSERIAL_PORT_SETTINGS)

        if url.startswith('socket://'):
            # interCharTimeout doesn't apply to sockets, so shrink the receive
            # timeout to compensate.
            ser.timeout = 0.5
            ser._socket.settimeout(0.5)

        return cls(ser, capture_stream)

    def connect(self, protocol):
        '''Open a link-layer socket for sending and receiving packets
        of a specific protocol number.
        '''
        if protocol in self.sockets and not self.sockets[protocol].closed:
            raise ValueError('A socket is already bound '
                             'to protocol 0x%04x' % protocol)
        self.sockets[protocol] = socket = InterfaceSocket(self, protocol)
        return socket

    def unregister_socket(self, protocol):
        '''Used by InterfaceSocket objects to unregister themselves when
        closing.
        '''
        try:
            del self.sockets[protocol]
        except KeyError:
            pass

    def receive_loop(self):
        splitter = framing.FrameSplitter()
        while True:
            if self.closed:
                self.logger.info('Interface closed; receive loop exiting')
                break
            try:
                splitter.write(self.iostream.read(1))
            except IOError:
                if self.closed:
                    self.logger.info('Interface closed; receive loop exiting')
                else:
                    self.logger.exception('Unexpected error while reading '
                                          'from iostream')
                    self._down()
                break

            for frame in splitter:
                try:
                    datagram = framing.decode_frame(frame)
                    if self.pcap:
                        # Prepend pseudo-header meaning "received by this host"
                        self.pcap.write_packet(b'\0' + datagram)
                    protocol, information = ppp.unencapsulate(datagram)
                    if protocol in self.sockets:
                        self.sockets[protocol].handle_packet(information)
                    else:
                        # TODO LCP Protocol-Reject
                        self.logger.info('Protocol-reject: %04X', protocol)
                except (framing.DecodeError, framing.CorruptFrame):
                    pass

    def send_packet(self, protocol, packet):
        if self.closed:
            raise ValueError('I/O operation on closed interface')
        datagram = ppp.encapsulate(protocol, packet)
        if self.pcap:
            # Prepend pseudo-header meaning "sent by this host"
            self.pcap.write_packet(b'\x01' + datagram)
        self.iostream.write(framing.encode_frame(datagram))

    def close_all_sockets(self):
        # Iterating over a copy of sockets since socket.close() can call
        # unregister_socket, which modifies the socket dict.  Modifying
        # a dict during iteration is not allowed, so the iteration is
        # completed (by making the copy) before modification can begin.
        for socket in list(self.sockets.values()):
            socket.close()

    def close(self):
        with self.close_lock:
            if self.closed:
                return
            self.lcp.shutdown()
            self.close_all_sockets()
            self._down()

            if self.pcap:
                self.pcap.close()

    def _down(self):
        '''The lower layer (iostream) is down. Bring down the interface.
        '''
        with self.close_lock:
            self.closed = True
            self.close_all_sockets()
            self.lcp.down()
            self.simplex_transport.down()
            self.iostream.close()

    def on_link_up(self):
        # FIXME PBL-34320 proper MTU/MRU support
        self._link = Link(self, mtu=1500)
        # Test whether the link is ready to carry traffic
        self.lcp.ping(self._ping_done)

    def _ping_done(self, ping_check_succeeded):
        if ping_check_succeeded:
            self.link_available.set()
        else:
            self.lcp.restart()

    def on_link_down(self):
        self.link_available.clear()
        self._link.down()
        self._link = None

    def get_link(self, timeout=60.0):
        '''Get the opened Link object for this interface.

        This function will block waiting for the Link to be available.
        It will return `None` if the timeout expires before the link
        is available.
        '''
        if self.closed:
            raise ValueError('No link available on closed interface')
        if self.link_available.wait(timeout):
            assert self._link is not None
            return self._link


class InterfaceSocket(object):
    '''A socket for sending and receiving link-layer packets over a
    PULSE2 interface.

    Callbacks can be registered on the socket by assigning callables to
    the appropriate attributes on the socket object. Callbacks can be
    unregistered by setting the attributes back to `None`.

    Available callbacks:
      - `on_packet(information)`
      - `on_close()`
    '''

    on_packet = None
    on_close = None

    def __init__(self, interface, protocol):
        self.interface = interface
        self.protocol = protocol
        self.closed = False

    def __enter__(self):
        return self

    def __exit__(self, type, value, traceback):
        self.close()

    def send(self, information):
        if self.closed:
            raise exceptions.SocketClosed('I/O operation on closed socket')
        self.interface.send_packet(self.protocol, information)

    def handle_packet(self, information):
        if self.on_packet and not self.closed:
            self.on_packet(information)

    def close(self):
        if self.closed:
            return
        self.closed = True
        if self.on_close:
            self.on_close()
        self.interface.unregister_socket(self.protocol)
        self.on_packet = None
        self.on_close = None


class Link(object):
    '''The connectionful portion of a PULSE2 interface.
    '''

    TRANSPORTS = {}

    on_close = None

    @classmethod
    def register_transport(cls, name, factory):
        '''Register a PULSE transport.
        '''
        if name in cls.TRANSPORTS:
            raise ValueError('transport name %r is already registered '
                             'with %r' % (name, cls.TRANSPORTS[name]))
        cls.TRANSPORTS[name] = factory

    def __init__(self, interface, mtu):
        self.logger = pulse2_logging.TaggedAdapter(
                logger, {'tag': type(self).__name__})
        self.interface = interface
        self.closed = False
        self.mtu = mtu

        self.transports = {}
        for name, factory in self.TRANSPORTS.iteritems():
            transport = factory(interface, mtu)
            self.transports[name] = transport

    def open_socket(self, transport, port, timeout=30.0):
        if self.closed:
            raise ValueError('Cannot open socket on closed Link')
        if transport not in self.transports:
            raise KeyError('Unknown transport %r' % transport)
        return self.transports[transport].open_socket(port, timeout)

    def down(self):
        self.closed = True
        if self.on_close:
            self.on_close()
        for transport in self.transports.itervalues():
            transport.down()
