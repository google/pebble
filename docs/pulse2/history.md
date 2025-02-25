History of PULSEv2
==================

This document describes the history of the Pebble dbgserial console
leading up to the design of PULSEv2.

In The Beginning
----------------

In the early days of Pebble, the dbgserial port was used to print out
log messages in order to assist in debugging the firmware. These logs
were plain text and could be viewed with a terminal emulator such as
minicom. An interactive prompt was added so that firmware developers and
the manufacturing line could interact with the running firmware. The
prompt mode could be accessed by pressing CTRL-C at the terminal, and
could be exited by pressing CTRL-D. Switching the console to prompt mode
suppressed the printing of log messages. Data could be written into the
external flash memory over the console port by running a prompt command
to switch the console to a special "flash imaging" mode and sending it
base64-encoded data.

This setup worked well enough, though it was slow and a little
cumbersome to use at times. Some hacks were tacked on as time went on,
like a "hybrid" prompt mode which allowed commands to be executed
without suppressing log messages. These hacks didn't work terribly well.
But it didn't really matter as the prompt was only used internally and
it was good enough to let people get stuff done.

First Signs of Trouble
----------------------

The problems with the serial console started becoming apparent when
we started building out automated integration testing. The test
automation infrastructure made extensive use of the serial console to
issue commands to simulate actions such as button clicks, inspect the
firmware state, install applications, and capture screenshots and log
messages. From the very beginning the serial console proved to be very
unreliable for test automation's uses, dropping commands, corrupting
screenshots and other data, and hiding log messages. The test automation
harness which interacted with the dbgserial port became full of hacks
and workarounds, but was still very unreliable. While we wanted to have
functional and reliable automated testing, we didn't have the manpower
at the time to improve the serial console for test automation's use
cases. And so test automation remained frustratingly unreliable for a
long time.

PULSEv1
-------

During the development of Pebble Time, the factory was complaining that
imaging the recovery firmware onto external flash over the dbgserial
port was taking too long and was causing a manufacturing bottleneck. The
old flash imaging mode had many issues and was in need of a replacement
anyway, and improving the throughput to reduce manufacturing costs
finally motivated us to allocate engineering time to replace it.

The biggest reason the flash imaging protocol was so slow was that it
was extremely latency sensitive. After every 768 data bytes sent, the
sender was required to wait for the receiver to acknowledge the data
before continuing. USB-to-serial adapter ICs are used at both the
factory and by developers to interface the watches' dbgserial ports to
modern computers, and these adapters can add up to 16 ms latency to
communications in each direction. The vast majority of the flash imaging
time was wasted with the dbgserial port idle, waiting for the sender to
receive and respond to an acknowledgement.

There were other problems too, such as a lack of checksums. If line
noise (which wasn't uncommon at the factory) corrupted a byte into
another valid base64 character, the corruption would go unnoticed and be
written out to flash. It would only be after the writing was complete
that the integrity was verified, and the entire transfer would have to
be restarted from the beginning.

Instead of designing a new flash imaging protocol directly on top of the
raw dbgserial console, as the old flash imaging protocol did, a
link-layer protocol was designed which the new flash imaging protocol
would operate on top of. This new protocol, PULSE version 1, provided
best-effort multiprotocol datagram delivery with integrity assurance to
any applications built on top of it. That is, PULSE allowed
applications to send and receive packets over dbgserial, without
interfering with other applications simultaneously using the link, with
the guarantee that the packets either will arrive at the receiver intact
or not be delivered at all. It was designed around the use-case of flash
imaging, with the hope that other protocols could be implemented over
PULSE later on. The hope was that this was the first step to making test
automation reliable.

Flash imaging turns out to be rather unique, with affordances that make
it easy to implement a performant protocol without protocol features
that many other applications would require. Writing to flash memory is
an idempotent operation: writing the same bytes to the same flash
address _n_ times has the same effect as writing it just once. And
writes to different addresses can be performed in any order. Because
of these features of flash, each write operation can be treated as a
wholly independent operation, and the data written to flash will be
complete as long as every write is performed at least once. The
communications channel for flash writes does not need to be reliable,
only error-free. The protocol is simple: send a write command packet
with the target address and data. The receiver performs the write and
sends an acknowledgement with the address. If the sender doesn't receive
an acknowledgement within some timeout, it re-sends the write command.
Any number of write commands and acknowledgements can be in-flight
simultaneously. If a write completes but the acknowledgement is lost in
transit, the sender can re-send the same write command and the receiver
can naively overwrite the data without issue due to the idempotence of
flash writes.

The new PULSE flash imaging protocol was a great success, reducing
imaging time from over sixty seconds down to ten, with the bottleneck
being the speed at which the flash memory could be erased or written.
After the success of PULSE flash imaging, attempts were made to
implement other protocols on top of it, with varying degrees of success.
A protocol for streaming log messages over PULSE was implemented, as
well as a protocol for reading data from external flash. There were
attempts to implement prompt commands and even an RPC system using
dynamically-loaded binary modules over PULSE, but they required reliable
and in-order delivery, and implementing a reliable transmission scheme
separately for each application protocol proved to be very
time-consuming and bug-prone.

Other flaws in PULSE became apparent as it came into wider use. The
checksum used to protect the integrity of PULSE frames was discovered to
have a serious flaw, where up to three trailing 0x00 bytes could be
appended to or dropped from a packet without changing the checksum
value. This flaw, combined with the lack of explicit length fields in
the protocol headers, made it much more likely for PULSE flash imaging
to write corrupted data. This was discovered shortly after test
automation switched over to PULSE flash imaging.

Make TA Green Again
-------------------

Around January 2016, it was decided that the issues with PULSE that were
preventing test automation from fully dropping use of the legacy serial
console would best be resolved by taking the lessons learned from PULSE
and designing a successor. This new protocol suite, appropriately
enough, is called PULSEv2. It is designed with test automation in mind,
with the intention of completely replacing the legacy serial console for
test automation, developers and the factory. It is much better at
communicating and synchronizing link state, which solves problems that
test automation was running into with the firmware crashing and
rebooting getting the test harness confused. It uses a standard checksum
without the flaws of its predecessor, and packet lengths are explicit.
And it is future-proofed by having an option-negotiation mechanism,
allowing us to add new features to the protocol while allowing old and
new implementations to interoperate.

Applications can choose to communicate with either best-effort datagram
service (like PULSEv1), or reliable datagram service that guarantees
in-order datagram delivery. Having the reliable transport available
made it very easy to implement prompt commands over PULSEv2. And it was
also surprisingly easy to implement a PULSEv2 transport for the Pebble
Protocol, which allows developers and test automation to interact with
bigboards using libpebble2 and pebble-tool, exactly like they can with
emulators and sealed watches connected to phones.

Test automation switched over to PULSEv2 on 2016 May 31. It immediately
cut down test run times and, once some bugs got shaken out, measurably
improved the reliability of test automation. It also made the captured
logs from test runs much more useful as messages were no longer getting
dropped. PULSEv2 was made the default for all firmware developers at the
end of September 2016.


<!-- vim: set tw=72: -->
