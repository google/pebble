#!/user/bin/env python
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


# For background information and high-level documentation on what this script does, see:
# https://docs.google.com/document/d/1e3e9KJtp9P9GJrqVm7TofnA-S9LrXlGAN6KDXSGvJys/edit

import argparse
import json
import os
import struct


EXPORTED_SYMBOLS_PATH = os.path.join(os.path.dirname(__file__),
                                     'generate_native_sdk/exported_symbols.json')
PRESERVE_SECTIONS = [
    '.text',
    '.debug_frame',
    '.shstrtab',
    '.symtab',
    '.strtab',
]

PRESERVE_SYMBOLS = [
    'g_app_load_address',
    'g_worker_load_address',
    'app_crashed',
]

# I have NO idea why we need to preserve `g_default_draw_implementation`, but we do. It's bizarre.
# We can at least obfuscate the name.
OBFUSCATE_SYMBOLS = [
    'g_default_draw_implementation',
]


class ELFObjectBase(object):
    def unpack(self, data, offset=0):
        raise NotImplementedError('unpack is not implement')

    def pack(self, data, offset=0):
        raise NotImplementedError('pack is not implemented')


class ELFFileHeader(ELFObjectBase):
    _STRUCT_FMT = '<4sBBBBBxxxxxxxHHIIIIIHHHHHH'

    MAGIC = '\x7fELF'
    CLASS_32_BIT = 1
    DATA_2_LSB = 1
    VERSION = 1
    OS_ABI = 0
    ABI_VERSION = 0
    MACHINE_ARM = 40

    magic = None  # The magic is '\x7fELF' for all ELF files
    elf_class = None  # The class of the ELF file (whether it's 32-bit or 64-bit)
    data = None  # The format of the data in the ELF file (endianness)
    version = None  # The version of the ELF file format
    osabi = None  # The OS- or ABI-specific extensions used in this ELF file
    abi_version = None  # The version of the ABI this file is targeted for
    type = None  # The object file type
    machine = None  # The machine architecture
    entry = None  # The program entry point
    ph_offset = None  # The offset of the program header table in bytes
    sh_offset = None  # The offset of the section header table in bytes
    flags = None  # The processor-specific flags
    size = None  # The size of this header in bytes
    ph_entry_size = None  # The size of the program header table entries in bytes
    ph_num = None  # The number of entries in the program header table
    sh_entry_size = None  # The size of the section header table entries in bytes
    sh_num = None  # The number of entries in the section header table
    sh_str_index = None  # The section header table index of the section name string table entry

    def validate(self):
        assert(self.magic == self.MAGIC)
        # we only support 32bit files
        assert(self.elf_class == self.CLASS_32_BIT)
        # we only support little-endian files
        assert(self.data == self.DATA_2_LSB)
        # current ELF verison
        assert(self.version == self.VERSION)
        assert(self.osabi == self.OS_ABI)
        assert(self.abi_version == self.ABI_VERSION)
        # we only support ARM
        assert(self.machine == self.MACHINE_ARM)
        assert(self.size == struct.calcsize(self._STRUCT_FMT))

    def unpack(self, data, offset=0):
        fields = struct.unpack_from(self._STRUCT_FMT, data, offset)
        (self.magic, self.elf_class, self.data, self.version, self.osabi, self.abi_version,
         self.type, self.machine, self.version, self.entry, self.ph_offset, self.sh_offset,
         self.flags, self.size, self.ph_entry_size, self.ph_num, self.sh_entry_size, self.sh_num,
         self.sh_str_index) = fields
        self.validate()

    def pack(self, data, offset=0):
        self.validate()
        fields = (self.magic, self.elf_class, self.data, self.version, self.osabi,
                  self.abi_version, self.type, self.machine, self.version, self.entry,
                  self.ph_offset, self.sh_offset, self.flags, self.size, self.ph_entry_size,
                  self.ph_num, self.sh_entry_size, self.sh_num, self.sh_str_index)
        struct.pack_into(self._STRUCT_FMT, data, offset, *fields)

    def __repr__(self):
        fields = (self.magic, self.elf_class, self.data, self.version, self.osabi,
                  self.abi_version, self.type, self.machine, self.version, self.entry,
                  self.ph_offset, self.sh_offset, self.flags, self.size, self.ph_entry_size,
                  self.ph_num, self.sh_entry_size, self.sh_num, self.sh_str_index)
        return '<{}@{}: magic=\'{}\', elf_class={}, data={}, version={}, osabi={}, ' \
               'abi_version={}, type={}, machine={}, version={}, entry={}, ph_offset={}, ' \
               'sh_offset={}, flags={}, size={}, ph_entry_size={}, ph_num={}, sh_entry_size={}, ' \
               'sh_num={}, sh_str_index={}>' \
               .format(self.__class__.__name__, hex(id(self)), *fields)


class ELFSectionHeader(ELFObjectBase):
    _STRUCT_FMT = '<IIIIIIIIII'

    TYPE_SYMBOL_TABLE = 2
    TYPE_STRING_TABLE = 3

    name = None  # The section name as an index into the section name table
    type = None  # The type of the section
    flags = None  # Section flags
    addr = None  # Address of this section at execution
    offset = None  # The offset of the section in bytes
    size = None  # The size of the section in bytes
    link = None  # The section header table link (interpretation various based on type)
    info = None  # Extra info
    addr_align = None  # Address alignment constraint for the section
    entry_size = None  # The size of entries within the section in bytes (if applicable)

    def unpack(self, data, offset=0):
        fields = struct.unpack_from(self._STRUCT_FMT, data, offset)
        (self.name, self.type, self.flags, self.addr, self.offset, self.size, self.link, self.info,
         self.addr_align, self.entry_size) = fields

    def pack(self, data, offset=0):
        fields = (self.name, self.type, self.flags, self.addr, self.offset, self.size, self.link,
                  self.info, self.addr_align, self.entry_size)
        struct.pack_into(self._STRUCT_FMT, data, offset, *fields)

    def __repr__(self):
        fields = (self.name, self.type, self.flags, self.addr, self.offset, self.size, self.link,
                  self.info, self.addr_align, self.entry_size)
        return '<{}@{}: name={}, type={}, flags={}, addr={}, offset={}, size={}, link={}, ' \
               'info={}, addr_align={}, entry_size={}>' \
               .format(self.__class__.__name__, hex(id(self)), *fields)


class ELFSection(ELFObjectBase):
    def __init__(self, header, content):
        self.header = header
        self.content = content
        self.name = ''


class ELFStringTable(ELFSection):
    def __init__(self, header, content):
        assert(header.type == header.TYPE_STRING_TABLE)
        assert(content[0] == 0 and content[-1] == 0)
        super(ELFStringTable, self).__init__(header, content)
        self.strings = []

    def unpack(self):
        string = bytearray()
        for i, b in enumerate(self.content):
            if b == 0:
                self.strings += [str(string)]
                string = bytearray()
            else:
                string += chr(b)

    def pack(self):
        self.content = bytearray('\0' + '\0'.join(self.strings) + '\0')

    def set_strings(self, strings):
        self.strings = strings

    def get_index(self, string):
        try:
            return self.content.index('\0' + string + '\0') + 1
        except ValueError:
            return None

    def get_string(self, offset):
        return str(self.content[offset:self.content.index('\0', offset)])


class ELFSymbolTable(ELFSection):
    def __init__(self, header, content):
        assert(header.type == header.TYPE_SYMBOL_TABLE)
        assert(len(content) % header.entry_size == 0)
        super(ELFSymbolTable, self).__init__(header, content)
        self.symbols = []

    def unpack(self):
        num_entries = len(self.content) / self.header.entry_size
        offset = 0
        for i in range(num_entries):
            entry = ELFSymbolTableEntry(self.header.entry_size)
            entry.unpack(self.content, i * self.header.entry_size)
            self.symbols.append(entry)
            offset += self.header.entry_size

    def pack(self):
        self.content = bytearray(len(self.symbols) * self.header.entry_size)
        offset = 0
        for symbol in self.symbols:
            symbol.pack(self.content, offset)
            offset += self.header.entry_size


class ELFSymbolTableEntry(ELFObjectBase):
    _STRUCT_FMT = '<IIIBBH'

    name = None  # The symbol name as an index into the symbol name table
    addr = None  # The address of the symbol
    size = None  # The size associated with the symbol
    info = None  # The binding and type of the symbol
    other = None  # The visibility of the symbol
    sh_idx = None  # The index into the section header table of the section this symbol relates to

    name_str = None  # A cache of the name of this symbol from the string table

    def __init__(self, entry_size):
        assert(struct.calcsize(self._STRUCT_FMT) == entry_size)

    def unpack(self, data, offset=0):
        fields = struct.unpack_from(self._STRUCT_FMT, data, offset)
        (self.name, self.addr, self.size, self.info, self.other, self.sh_idx) = fields

    def pack(self, data, offset=0):
        fields = (self.name, self.addr, self.size, self.info, self.other, self.sh_idx)
        struct.pack_into(self._STRUCT_FMT, data, offset, *fields)

    def __repr__(self):
        fields = (self.name, self.addr, self.size, self.info, self.other, self.sh_idx)
        return '<{}@{}: name={}, addr={}, size={}, info={}, other={}, sh_idx={}>' \
               .format(self.__class__.__name__, hex(id(self)), *fields)


class ELFFile(ELFObjectBase):
    def __init__(self):
        self.file_header = ELFFileHeader()
        self.sections = []

    def unpack(self, raw_data):
        # get the file header
        self.file_header.unpack(raw_data)

        # get the sections
        for i in range(self.file_header.sh_num):
            header = ELFSectionHeader()
            header.unpack(raw_data, self.file_header.sh_offset + i * self.file_header.sh_entry_size)
            content = raw_data[header.offset:header.offset+header.size]
            if header.type == header.TYPE_SYMBOL_TABLE:
                section = ELFSymbolTable(header, content)
                section.unpack()
            elif header.type == header.TYPE_STRING_TABLE:
                section = ELFStringTable(header, content)
                section.unpack()
            else:
                section = ELFSection(header, content)
            self.sections.append(section)

        # populate the section names
        shstrtab_section = self.sections[self.file_header.sh_str_index]
        for section in self.sections:
            section.name = shstrtab_section.get_string(section.header.name)

        # populate the symbol names
        strtab_section = self.get_section('.strtab')
        symtab_section = self.get_section('.symtab')
        for entry in symtab_section.symbols:
            entry.name_str = strtab_section.get_string(entry.name)

    def pack(self):
        # update the fields for the section header table
        self.file_header.sh_num = len(self.sections)
        self.file_header.sh_offset = self.file_header.size
        for section in self.sections:
            self.file_header.sh_offset += len(section.content)

        # we don't care about preserving the program header table
        self.file_header.ph_offset = 0
        self.file_header.ph_num = 0

        # grab the new shstrtab section header index
        self.file_header.sh_str_index = self.get_section_index('.shstrtab')

        # update the `link` property of .symtab to the new index of .strtab
        symtab_section = self.get_section('.symtab')
        symtab_section.header.link = self.get_section_index('.strtab')
        symtab_section.header.info = 0

        # we're now ready to rebuild the raw binary data
        raw_data = bytearray(self.file_header.sh_offset +
                             (len(self.sections) * self.file_header.sh_entry_size))
        offset = 0
        # add the file header
        self.file_header.pack(raw_data)
        offset += self.file_header.size
        # add the sections
        for section in self.sections:
            section.header.offset = offset
            section.header.size = len(section.content)
            for b in section.content:
                raw_data[offset] = b
                offset += 1
        # add the section headers
        for section in self.sections:
            section.header.pack(raw_data, offset)
            offset += self.file_header.sh_entry_size

        return raw_data

    def get_section_index(self, name):
        for index, section in enumerate(self.sections):
            if section.name == name:
                return index
        raise Exception('Could not find section: {}'.format(name))

    def get_section(self, name):
        for section in self.sections:
            if section.name == name:
                return section
        raise Exception('Could not find section: {}'.format(name))


def _get_preserved_symbols():
    """ This function builds a dictionary of symbols we want to preserve in the output .elf where
        the key is the existing symbol name and the value is the name it should be changed to."""
    preserved_symbols = {}

    # add the exported symbols
    def get_exported_symbols(exports, result, rev):
        for export in exports:
            if export['type'] == "function":
                if (any(k in export for k in ['internal', 'removed', 'deprecated']) or
                    export['addedRevision'] > rev):
                    continue

                internal_name = export.get('implName', export['name']).encode('ascii')
                external_name = export.get('sortName', export['name']).encode('ascii')
                result[internal_name] = external_name
            elif export['type'] == "group":
                get_exported_symbols(export['exports'], result, rev)

    with open(EXPORTED_SYMBOLS_PATH) as f:
        data = json.loads(f.read())
        get_exported_symbols(data['exports'], preserved_symbols, data['revision'])

    # add the symbols we're keeping intact
    for symbol_name in PRESERVE_SYMBOLS:
        preserved_symbols[symbol_name] = symbol_name

    # add the symbols we're obfuscating (by renaming them to '??')
    for symbol_name in OBFUSCATE_SYMBOLS:
        preserved_symbols[symbol_name] = '??'

    return preserved_symbols


def obfuscate(src_path, dst_path, no_text):
    elf = ELFFile()
    with open(src_path, 'rb') as f:
        elf.unpack(bytearray(f.read()))

    # build a "<IMPL_NAME>" -> "<OUTPUT_NAME>" dictionary of the symbols we want to keep
    preserved_symbols = _get_preserved_symbols()

    # grab the original index of the .text section
    old_text_index = elf.get_section_index('.text')

    # filter the sections
    new_sections = []
    shstrtab_section = elf.get_section('.shstrtab')
    shstrtab_section.set_strings(PRESERVE_SECTIONS)
    shstrtab_section.pack()
    for section in elf.sections:
        if section.name in PRESERVE_SECTIONS or section.name == '':
            new_sections.append(section)
            if section.name != '':
                section.header.name = shstrtab_section.get_index(section.name)
    elf.sections = new_sections

    # repopulate the string table with just the symbols we want to preserve
    strtab_section = elf.get_section('.strtab')
    strtab_section.set_strings(preserved_symbols.values())
    strtab_section.pack()

    # filter the symbols
    symtab_section = elf.get_section('.symtab')
    # The ELF symbol table must start with the NULL symbol, so copy it over.
    new_symbols = [symtab_section.symbols[0]]
    new_text_index = 0 if no_text else elf.get_section_index('.text')
    for entry in symtab_section.symbols:
        # fix the section header index for the .text section, or set the
        # section to SHN_ABS (0xfff1) so that GDB will acknowledge that the
        # symbol exists.
        entry.sh_idx = new_text_index if entry.sh_idx == old_text_index else 0xfff1
        # update the new symbol name offset
        name_str = preserved_symbols.get(entry.name_str, None)
        entry.name = strtab_section.get_index(name_str) if name_str else 0
        # if the name isn't in the string table, we don't want to include this symbol at all
        if entry.name:
            new_symbols += [entry]

    # rebuild the symbol table
    symtab_section.symbols = new_symbols
    symtab_section.pack()

    # NOTE: We currently aren't making any modifications to the .debug_frame section. If we want to
    # investigate doing so in the future, there is some python code to decode it here:
    # https://gist.github.com/bgomberg/4bcb49a7b82071fc6cd3

    with open(dst_path, 'wb') as f:
        f.write(elf.pack())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Pebble Firmware ELF Obfuscation')
    parser.add_argument('input_elf', help='The source ELF file to be obfuscaated')
    parser.add_argument('output_elf', help='Output file path')
    parser.add_argument('--no-text', help='Removes the .text section', action='store_true')
    args = parser.parse_args()

    if args.no_text:
        PRESERVE_SECTIONS.remove('.text')

    obfuscate(args.input_elf, args.output_elf, args.no_text)
