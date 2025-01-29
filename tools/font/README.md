Pebble Font Renderer Script
===========================

These Python scripts take TrueType font files, renders a set of glyphs and outputs them into .h files in the appropriate structure for consumption by Pebble's text rendering routines.

Requirements:
-------------
* freetype library
* freetype-py binding

http://code.google.com/p/freetype-py/

**Mac OS X and freetype-py**: the freetype binding works with the Freetype library that ships with Mac OS X (/usr/X11/lib/libfreetype.dylib), but you need to patch setup.py using this diff file:

https://gist.github.com/3345193
