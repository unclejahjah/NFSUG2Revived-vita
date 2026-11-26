"""The game executable as the generator sees it: sections at their preferred addresses, base relocations, imports.

The executable is never rebased; the runtime loads the same file at the same addresses. A game DLL (server.dll) is
read as loaded at the guest base the runtime maps it at: its own base relocations applied, as the runtime applies
them to the original file. Every import gets a fake address in the header page below the code; the runtime writes these into the import address table and registers its shims
there, so an indirect call through the table reaches the shim.
"""

import hashlib
import struct

from . import pefile

# Fake import addresses: one per import, in the PE header page (the code starts at the end offset): offsets from the
# image base (SPEED2.EXE: 0x400a00-0x401000).
FAKE_IMPORT_OFFSET = 0xa00
FAKE_IMPORT_END_OFFSET = 0x1000

# Names of imports by ordinal (pefile's table does not know these DLLs).
ORDINALS = {
    ('dsound', 1): 'DirectSoundCreate',
    ('dsound', 2): 'DirectSoundEnumerateA',
}

IMAGE_REL_BASED_HIGHLOW = 3
IMAGE_SCN_CNT_CODE = 0x00000020
IMAGE_SCN_MEM_EXECUTE = 0x20000000
IMAGE_SCN_MEM_DISCARDABLE = 0x02000000
IMAGE_SCN_MEM_WRITE = 0x80000000


class Section:
    def __init__(self, name, address, virtual_size, file_offset, raw_size, data, code, writable, discardable):
        self.name = name                    # e.g. '.text'
        self.address = address              # guest address
        self.virtual_size = virtual_size
        self.file_offset = file_offset      # raw data position in the file (0: none)
        self.raw_size = raw_size            # bytes of raw data (the rest of the virtual size is zero)
        self.data = data                    # raw data (bytes)
        self.code = code
        self.writable = writable
        self.discardable = discardable

    @property
    def end(self):
        return self.address + max(self.virtual_size, self.raw_size)

    def contains(self, address):
        return self.address <= address < self.end


class Import:
    def __init__(self, dll, name, iat_address, fake_address):
        self.dll = dll                      # lower case without .dll, e.g. 'kernel32'
        self.name = name
        self.iat_address = iat_address      # guest address of the import address table slot
        self.fake_address = fake_address


class Image:
    def __init__(self, path, relocation_section=None, rebase=None):
        """relocation_section: the section holding relocation blocks the directory lost. rebase: the guest base the
        image is loaded at instead of its preferred one (its base relocations applied)."""
        self.path = path
        with open(path, 'rb') as f:
            self.file_bytes = f.read()
        self.sha256 = hashlib.sha256(self.file_bytes).hexdigest()
        pe = pefile.PE(data=self.file_bytes)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.entry_point = self.base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.size_of_image = pe.OPTIONAL_HEADER.SizeOfImage
        self.header_size = pe.OPTIONAL_HEADER.SizeOfHeaders
        self.sections = []
        for s in pe.sections:
            flags = s.Characteristics
            self.sections.append(Section(
                s.Name.rstrip(b'\0').decode(), self.base + s.VirtualAddress, s.Misc_VirtualSize,
                s.PointerToRawData, s.SizeOfRawData, s.get_data()[:s.SizeOfRawData],
                bool(flags & (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE)), bool(flags & IMAGE_SCN_MEM_WRITE),
                bool(flags & IMAGE_SCN_MEM_DISCARDABLE)))
        self.relocations = set()
        for block in getattr(pe, 'DIRECTORY_ENTRY_BASERELOC', []):
            for entry in block.entries:
                if entry.type == IMAGE_REL_BASED_HIGHLOW:
                    self.relocations.add(self.base + entry.rva)
                elif entry.type != 0:
                    raise AssertionError('relocation type %d at 0x%x' % (entry.type, self.base + entry.rva))
        if not self.relocations and relocation_section is not None:
            self.relocations = self.relocation_blocks(relocation_section)
        self.preferred_base = self.base
        delta = 0
        if rebase is not None and rebase != self.base:
            if not self.relocations:
                raise AssertionError('no base relocations: the image cannot move to 0x%x' % rebase)
            delta = (rebase - self.base) & 0xffffffff
            self.rebase(rebase)
        self.imports = []
        fake = self.base + FAKE_IMPORT_OFFSET
        fake_end = self.base + FAKE_IMPORT_END_OFFSET
        for descriptor in pe.DIRECTORY_ENTRY_IMPORT:
            dll = descriptor.dll.decode().lower()
            if dll.endswith('.dll'):
                dll = dll[:-4]
            for imp in descriptor.imports:
                if imp.name:
                    name = imp.name.decode()
                else:
                    name = ORDINALS.get((dll, imp.ordinal))
                    if not name:
                        raise AssertionError('unknown import %s ordinal %d' % (dll, imp.ordinal))
                if fake >= fake_end:
                    raise AssertionError('fake import addresses reach 0x%x' % fake_end)
                self.imports.append(Import(dll, name, (imp.address + delta) & 0xffffffff, fake))
                fake += 1
        self.code_sections = [s for s in self.sections if s.code]
        self.exports = []                   # (name, guest address) of a DLL's named exports
        for e in getattr(getattr(pe, 'DIRECTORY_ENTRY_EXPORT', None), 'symbols', []):
            if e.name:
                self.exports.append((e.name.decode(), (self.preferred_base + e.address + delta) & 0xffffffff))

    def rebase(self, base):
        """Moves the image to base: every relocated dword gets the difference, as the Windows loader does."""
        delta = (base - self.base) & 0xffffffff
        for site in sorted(self.relocations):
            s = self.section_of(site)
            offset = site - s.address
            if offset + 4 > len(s.data):
                raise AssertionError('relocated dword at 0x%x past the raw data' % site)
            value = (int.from_bytes(s.data[offset:offset + 4], 'little') + delta) & 0xffffffff
            s.data = s.data[:offset] + value.to_bytes(4, 'little') + s.data[offset + 4:]
        for s in self.sections:
            s.address = (s.address + delta) & 0xffffffff
        self.relocations = set((a + delta) & 0xffffffff for a in self.relocations)
        self.entry_point = (self.entry_point + delta) & 0xffffffff
        self.base = base

    def relocation_blocks(self, address):
        """HIGHLOW sites of the IMAGE_BASE_RELOCATION chain starting a section."""
        s = self.section_of(address)
        if s is None or s.address != address:
            raise AssertionError('no section starts at 0x%x' % address)
        data = s.data
        sites = set()
        offset = 0
        while offset + 8 <= len(data):
            page, size = struct.unpack_from('<II', data, offset)
            if not size:
                break
            if size < 8 or size % 4 or page % 0x1000 or page >= self.size_of_image or offset + size > len(data):
                raise AssertionError('relocation block at 0x%x: page 0x%x, size %d' % (address + offset, page, size))
            for k in range(offset + 8, offset + size, 2):
                (entry,) = struct.unpack_from('<H', data, k)
                if entry >> 12 == IMAGE_REL_BASED_HIGHLOW:
                    sites.add(self.base + page + (entry & 0xfff))
                elif entry >> 12:
                    raise AssertionError('relocation type %d in the block at 0x%x' % (entry >> 12, address + offset))
            offset += size
        if any(data[offset:]):
            raise AssertionError('bytes after the relocation chain at 0x%x' % (address + offset))
        for site in sites:
            value = self.dword(site)
            if value is None or not self.base <= value < self.base + self.size_of_image:
                raise AssertionError('relocated dword at 0x%x holds no image address' % site)
        return sites

    def section_of(self, address):
        for s in self.sections:
            if s.contains(address):
                return s
        return None

    def is_code(self, address):
        return any(s.contains(address) for s in self.code_sections)

    def read(self, address, size):
        """Bytes at address; bytes past the raw data read as zero, outside every section as nothing."""
        s = self.section_of(address)
        if s is None:
            return b''
        offset = address - s.address
        end = min(offset + size, s.end - s.address)
        return s.data[offset:end].ljust(end - offset, b'\0')

    def dword(self, address):
        data = self.read(address, 4)
        return int.from_bytes(data, 'little') if len(data) == 4 else None

    def end_address(self):
        return max(s.end for s in self.sections)
