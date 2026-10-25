"""Echo VR data files, as EchoLazyTablet's installer needs them (win10 build, manifest 48037dc70b0ecab2).

Written for EchoLazyTablet; needs `zstandard`.

Symbols   64-bit names: a CRC-64 style hash of the lowercased ASCII name (CSymbol64). A resource
          type is the hash of "Win10" (or "Win10GPU") seeded with the hash of the class name.
Container 0x18-byte header ("ZSTD"/"NONE", u32, u64 size, u64 compressed size), then the payload.
Manifest  u32 package count, u32 max block, then three arrays, each a 56-byte descriptor (byte size
          at +8, capacity at +0x28, count at +0x30) and, for the first two, 8 spare bytes:
            entries  0x20: type, name, location (frame | offset in frame << 32), size, alignment
            hashes   0x28: type, name, two hash words, build stamp (same order as entries)
            frames   0x10: package, file offset, compressed size, size; each package ends with a
                     marker frame (offset = package size, sizes 0)
          Entries and hashes are kept sorted by (type, name) as signed numbers.
Packages  `<manifest>_<n>`: zstd frames back to back.
Canvas    CUICanvasResource: a 432-byte header with array descriptors (count at +0x30, alignment
          at +0x18) for elements (224 B), behaviours (88 B, each with its own payload), atlas
          (24 B), text offsets (4 B), text (1 B) and bindings (24 B), stored in that order, each
          aligned. Element: +0 name, +8 kind (1 text, 2 panel, 3 sprite), +0x10 hidden,
          +0x34 rect (4 floats), +0x5c parent, +0x60 index, +0x78 colour, +0x88 texture,
          +0x90 UVs (sprite) or text slot + capacity (text).
Records   Fixed-size component tables (CR15ButtonInteractCR): a 0x38-byte header whose descriptor
          holds the byte size (+8) and count (+0x28, +0x30), then the records.
"""
import hashlib
import struct
from pathlib import Path

import zstandard

MID = '48037dc70b0ecab2'
MASK = (1 << 64) - 1
_POLY = 0x95AC9329AC4BC9B5


def _table():
    # Byte-at-a-time table of the game's symbol hash (polynomial above, MSB first, the top two
    # bits of each byte folded in specially, as the game builds it).
    table = []
    for i in range(256):
        v = (_POLY << 1) & MASK if i & 0x80 else 0
        if i & 0x40:
            v = 0xBEF5B57AF4DC5ADF if i & 0x80 else _POLY
        for bit in (0x20, 0x10, 0x08, 0x04, 0x02, 0x01):
            v = ((v << 1) ^ _POLY) & MASK if i & bit else (v << 1) & MASK
        table.append((v << 1) & MASK)
    return table


_TABLE = _table()


def sym(name, seed=MASK):
    value = seed
    for byte in name.encode('ascii'):
        if 65 <= byte <= 90:
            byte += 32
        value = (_TABLE[value >> 56] ^ byte ^ (value << 8)) & MASK
    return value


def typesym(class_name, gpu=False):
    return sym('Win10GPU' if gpu else 'Win10', seed=sym(class_name))


def _signed(x):
    return x - (1 << 64) if x >> 63 else x


def unpack_container(raw):
    tag = raw[:4]
    if tag == b'ZSTD':
        return zstandard.ZstdDecompressor().decompressobj().decompress(raw[0x18:])
    if tag == b'NONE':
        return raw[0x18:]
    raise ValueError(f'unknown container {tag!r}')


def sha256(data):
    return hashlib.sha256(data).hexdigest()


class MissingPackage(FileNotFoundError):
    """The manifest points at a package file that isn't in the game folder."""
    def __init__(self, package):
        super().__init__(f'package _{package} is missing')
        self.package = package


class Manifest:
    """The manifest, editable. `data` is the folder holding manifests/ and packages/."""

    def __init__(self, raw, data=None):
        self.raw = bytes(raw)
        self.data = Path(data) if data else None
        self.container = bytearray(raw[:0x18])
        body = unpack_container(raw)
        self.packages, self.max_block = struct.unpack_from('<II', body, 0)
        self.head = bytearray(body[:0xC0])
        counts = [struct.unpack_from('<Q', body, d + 0x30)[0] for d in (0x08, 0x48, 0x88)]
        at = 0xC0
        self.entries = [list(struct.unpack_from('<QQQII', body, at + i * 0x20)) for i in range(counts[0])]
        at += counts[0] * 0x20
        self.hashes = [list(struct.unpack_from('<QQQQQ', body, at + i * 0x28)) for i in range(counts[1])]
        at += counts[1] * 0x28
        self.frames = [list(struct.unpack_from('<IIII', body, at + i * 0x10)) for i in range(counts[2])]
        at += counts[2] * 0x10
        if at != len(body) or counts[0] != counts[1]:
            raise ValueError('manifest layout not as expected')
        self._cache = {}

    def users_of(self, package):
        """(type, name) of every resource stored in package `package`."""
        return [(e[0], e[1]) for e in self.entries if self.frames[e[2] & 0xFFFFFFFF][0] == package]

    @classmethod
    def load(cls, data):
        data = Path(data)
        return cls((data / 'manifests' / MID).read_bytes(), data)

    def find(self, type_sym, name_sym):
        for i, e in enumerate(self.entries):
            if e[0] == type_sym and e[1] == name_sym:
                return i
        return None

    def frame(self, index, extra_packages=None):
        if index not in self._cache:
            package, offset, csize, _ = self.frames[index]
            path = (extra_packages or {}).get(package) or self.data / 'packages' / f'{MID}_{package}'
            if not Path(path).is_file():
                raise MissingPackage(package)
            with open(path, 'rb') as f:
                f.seek(offset)
                block = f.read(csize)
            self._cache[index] = zstandard.ZstdDecompressor().decompressobj().decompress(block)
        return self._cache[index]

    def get(self, type_sym, name_sym, extra_packages=None):
        i = self.find(type_sym, name_sym)
        if i is None:
            return None
        _, _, loc, size, _ = self.entries[i]
        start = loc >> 32
        return self.frame(loc & 0xFFFFFFFF, extra_packages)[start:start + size]

    def package_of(self, type_sym, name_sym):
        i = self.find(type_sym, name_sym)
        return None if i is None else self.frames[self.entries[i][2] & 0xFFFFFFFF][0]

    def serialize(self):
        order = sorted(range(len(self.entries)), key=lambda i: (_signed(self.entries[i][0]), _signed(self.entries[i][1])))
        entries = [self.entries[i] for i in order]
        hashes = [self.hashes[i] for i in order]
        head = bytearray(self.head)
        struct.pack_into('<I', head, 0, self.packages)
        for d, count, stride in ((0x08, len(entries), 0x20), (0x48, len(hashes), 0x28), (0x88, len(self.frames), 0x10)):
            struct.pack_into('<Q', head, d + 8, count * stride)
            struct.pack_into('<QQ', head, d + 0x28, count, count)
        body = bytearray(head)
        for e in entries:
            body += struct.pack('<QQQII', *e)
        for h in hashes:
            body += struct.pack('<QQQQQ', *h)
        for f in self.frames:
            body += struct.pack('<IIII', *f)
        payload = zstandard.ZstdCompressor(level=3).compress(bytes(body))
        header = bytearray(self.container)
        header[:4] = b'ZSTD'
        struct.pack_into('<QQ', header, 8, len(body), len(payload))
        return bytes(header) + payload

    def add_package(self, resources):
        """Appends a package holding `resources` [(type, name, bytes)], one frame each, and points
        those resources at it (replacing or adding entries). -> (package number, package bytes)."""
        number = self.packages
        self.packages += 1
        blob = bytearray()
        compressor = zstandard.ZstdCompressor(level=3)
        stamp = self.hashes[0][4] if self.hashes else 0
        for type_sym, name_sym, data in resources:
            frame = compressor.compress(bytes(data))
            index = len(self.frames)
            self.frames.append([number, len(blob), len(frame), len(data)])
            blob += frame
            digest = hashlib.blake2b(bytes(data), digest_size=16).digest()
            entry = [type_sym, name_sym, index, len(data), 0x10]
            hashed = [type_sym, name_sym, *struct.unpack('<QQ', digest), stamp]
            i = self.find(type_sym, name_sym)
            if i is None:
                self.entries.append(entry)
                self.hashes.append(hashed)
            else:
                self.entries[i], self.hashes[i] = entry, hashed
        self.frames.append([number, len(blob), 0, 0])
        self._cache = {}
        return number, bytes(blob)


def _descriptor(header, off, count, stride):
    struct.pack_into('<Q', header, off + 8, count * stride)
    struct.pack_into('<QQ', header, off + 0x28, count, count)


class Canvas:
    HEADER = 432
    ARRAYS = ((0x50, 224, 8), (0x88, 88, 8), (0xC0, 24, 8), (0x100, 4, 4), (0x138, 1, 1), (0x170, 24, 8))

    def __init__(self, data):
        data = bytes(data)
        if len(data) < self.HEADER:
            raise ValueError('canvas too short')
        self.header = bytearray(data[:self.HEADER])
        self.gaps = {}
        pos = self.HEADER

        def take(key, size, align):
            nonlocal pos
            if not size:
                return bytearray()
            pad = (-pos) % align
            self.gaps[key] = data[pos:pos + pad]
            pos += pad
            chunk = bytearray(data[pos:pos + size])
            if len(chunk) != size:
                raise ValueError('canvas array cut short')
            pos += size
            return chunk

        def array(off, stride, align):
            count = struct.unpack_from('<Q', data, off + 0x30)[0]
            return take(off, count * stride, max(align, struct.unpack_from('<I', data, off + 0x18)[0]))

        rows = array(0x50, 224, 8)
        self.elements = [rows[i:i + 224] for i in range(0, len(rows), 224)]
        rows = array(0x88, 88, 8)
        self.behaviors = [rows[i:i + 88] for i in range(0, len(rows), 88)]
        self.behavior_data = [take(('b', i), struct.unpack_from('<Q', r, 0x18)[0], max(1, struct.unpack_from('<I', r, 0x28)[0]))
                              for i, r in enumerate(self.behaviors)]
        self.atlas = array(0xC0, 24, 8)
        self.text_offsets = array(0x100, 4, 4)
        self.text = array(0x138, 1, 1)
        self.bindings = array(0x170, 24, 8)
        if pos != len(data):
            raise ValueError(f'canvas has {len(data) - pos} unexpected trailing bytes')

    def serialize(self):
        out = bytearray(self.header)

        def put(key, payload, align):
            if not payload:
                return
            pad = (-len(out)) % align
            old = self.gaps.get(key, b'')
            out.extend(old if len(old) == pad else bytes(pad))
            out.extend(payload)

        def align_of(off, align):
            return max(align, struct.unpack_from('<I', self.header, off + 0x18)[0])

        put(0x50, b''.join(self.elements), align_of(0x50, 8))
        put(0x88, b''.join(self.behaviors), align_of(0x88, 8))
        for i, (row, payload) in enumerate(zip(self.behaviors, self.behavior_data)):
            put(('b', i), payload, max(1, struct.unpack_from('<I', row, 0x28)[0]))
        put(0xC0, self.atlas, align_of(0xC0, 8))
        put(0x100, self.text_offsets, align_of(0x100, 4))
        put(0x138, self.text, align_of(0x138, 1))
        put(0x170, self.bindings, align_of(0x170, 8))
        return bytes(out)

    @classmethod
    def empty(cls, donor, width, height):
        c = cls(donor)
        c.elements, c.behaviors, c.behavior_data = [], [], []
        c.atlas, c.text_offsets, c.text, c.bindings = bytearray(), bytearray(), bytearray(), bytearray()
        for off, stride, _ in cls.ARRAYS:
            _descriptor(c.header, off, 0, stride)
        struct.pack_into('<4I', c.header, 0x0C, width, height, width, height)
        return c

    # element fields
    @staticmethod
    def name(row):
        return struct.unpack_from('<Q', row, 0)[0]

    @staticmethod
    def kind(row):
        return struct.unpack_from('<I', row, 8)[0]

    @staticmethod
    def hidden(row):
        return struct.unpack_from('<I', row, 0x10)[0] != 0

    @staticmethod
    def rect(row):
        return list(struct.unpack_from('<4f', row, 0x34))

    @staticmethod
    def set_rect(row, rect):
        struct.pack_into('<4f', row, 0x34, *rect)

    @staticmethod
    def parent(row):
        return struct.unpack_from('<i', row, 0x5C)[0]

    def append(self, template, name, rect, parent=-1, hidden=False):
        row = bytearray(template)
        index = len(self.elements)
        struct.pack_into('<Q', row, 0, sym(name))
        struct.pack_into('<I', row, 0x10, int(hidden))
        struct.pack_into('<4f', row, 0x24, 0, 0, 0, 0)
        struct.pack_into('<4f', row, 0x34, *rect)
        struct.pack_into('<iI', row, 0x5C, parent, index)
        self.elements.append(row)
        _descriptor(self.header, 0x50, len(self.elements), 224)
        return index

    def label(self, template, name, words, rect, size=30, hidden=False, capacity=0):
        """A text element; `capacity` leaves room for longer text the runtime writes later."""
        index = self.append(template, name, rect, hidden=hidden)
        row = self.elements[index]
        encoded = words.encode('utf-8')
        capacity = max(64, len(encoded) + 1, capacity)
        slot = len(self.text_offsets) // 4
        self.text_offsets += struct.pack('<I', len(self.text))
        self.text += encoded + bytes(capacity - len(encoded))
        struct.pack_into('<II', row, 0x90, slot, capacity)
        struct.pack_into('<H', row, 0xA4, size)
        struct.pack_into('<4f', row, 0x78, 1, 1, 1, 1)
        row[0xC0:0xE0] = b'\xff' * 32       # literal text: no localisation / blackboard binding
        _descriptor(self.header, 0x100, slot + 1, 4)
        _descriptor(self.header, 0x138, len(self.text), 1)
        return index

    def check(self):
        for i, row in enumerate(self.elements):
            parent, index = struct.unpack_from('<iI', row, 0x5C)
            if index != i or not -1 <= parent < i:
                raise ValueError(f'element {i}: bad index or parent')
            if self.kind(row) == 1:
                slot, capacity = struct.unpack_from('<II', row, 0x90)
                if slot * 4 + 4 > len(self.text_offsets):
                    raise ValueError(f'element {i}: text slot out of range')
                if struct.unpack_from('<I', self.text_offsets, slot * 4)[0] + capacity > len(self.text):
                    raise ValueError(f'element {i}: text capacity out of range')
        for row, payload in zip(self.behaviors, self.behavior_data):
            if struct.unpack_from('<Q', row, 0x48)[0] >= len(self.elements) or struct.unpack_from('<Q', row, 0x18)[0] != len(payload):
                raise ValueError('behaviour out of range')
        if Canvas(self.serialize()).serialize() != self.serialize():
            raise ValueError('canvas does not read back the same')


class Records:
    """A fixed-size component record table."""

    def __init__(self, data):
        data = bytes(data)
        size, = struct.unpack_from('<Q', data, 8)
        count, = struct.unpack_from('<Q', data, 0x28)
        if not count or size % count or 0x38 + size != len(data):
            raise ValueError('not a fixed-size record table')
        self.header = bytearray(data[:0x38])
        self.stride = size // count
        self.rows = [bytes(data[0x38 + i * self.stride:0x38 + (i + 1) * self.stride]) for i in range(count)]

    def serialize(self):
        header = bytearray(self.header)
        _descriptor(header, 0, len(self.rows), self.stride)
        return bytes(header) + b''.join(self.rows)


def u64(data, off):
    return struct.unpack_from('<Q', data, off)[0]
