Default Header Overrides
========================

This directory contains header files which always override headers in
the firmware source tree when building unit tests. This is accomplished
by prepending this directory to the `#include` search path in
[`waftools/pebble_test.py`](../../../waftools/pebble_test.py).

Overriding the default override for a test?
-------------------------------------------

Overriding a default override header is very simple. Simply create a new
override tree and add it to your test's `clar()` build rule, just like
when overriding a header from the source tree. See the
[overrides README](../README.md) for details.

If you are considering adding a new override header
---------------------------------------------------

Only add new headers here if you are absolutely, **positively** sure
that it should always override the header of the same name in each and
every test unless explicitly overridden by yet another header in the
test's `clar()` build rule.

- Is the header being overridden used pervasively throughout the code,
  affecting the majority of tests?
- Does the header contain only inline function definitions and macros?
- Do the definitions of those functions and macros make no sense when
  compiled into a test, e.g. by containing inline ASM? Will the source
  files `#include`'ing these files always fail to compile unless
  overridden?
- Is using `#ifdef __ARM__` conditional compilation to replace
  ARM-specific code with a generic version insufficient? Would it require
  adding test-harness code to the source tree?

If the answer to any of the above questions is "no", then adding an
override header is the wrong solution. If the answer to all of the above
questions is "yes", then adding an override header *might* be the right
solution. Adding a new default override is an action which should not be
taken lightly; it should only be done if the benefits outweigh the
risks.

### Alternatives to writing a default override header ###

- Create a stub or fake header which is `#include`'ed into the test
  which requires it.
- Create a new override directory which individual tests can opt into.
