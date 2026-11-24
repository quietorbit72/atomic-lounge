# SPDX-License-Identifier: AGPL-3.0-only
# Vintage Chonk: reader (and minimal writer, for fixtures) of the Native Instruments
# "NI Sound" container ('hsin' magic) used by Massive .nmsv presets.
#
# Layout (little-endian), established from the bytes of a public test file and
# matching the community descriptions listed in docs/MASSIVE_PRESET_FORMAT.md:
#
#   Item        := Header(40) DataFrame Children
#   Header      := u64 length (whole item) | u32 version | 'hsin' | u32 flags |
#                  u32 reserved | 16-byte UUID
#   DataFrame   := u64 size | domain(4) | u32 item_id | u32 version |
#                  [nested DataFrame unless item_id == 1] | own data (to size)
#                  The outermost frame is the most derived type; id 1 is the
#                  base "Item" and ends the stack.
#   Children    := u32 version(1) | u32 count | count x
#                  (u32 index | domain(4) | u32 item_id | Item)
import os
import struct
import sys
import uuid as _uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fastlz import decompress, FastLZError  # noqa: E402

MAGIC = b"hsin"
DOMAIN_NISD = b"DSIN"   # 'NISD' as written

ITEM_NAMES = {
    0x01: "Item", 0x64: "Bank", 0x65: "Preset", 0x66: "BankContainer",
    0x67: "PresetContainer", 0x68: "BinaryChunkItem", 0x6A: "Authorization",
    0x6C: "SoundInfoItem", 0x6D: "PresetChunkItem", 0x6E: "ExternalFileReference",
    0x6F: "Resources", 0x70: "AudioSampleItem", 0x71: "InternalResourceReferenceItem",
    0x72: "PictureItem", 0x73: "SubtreeItem", 0x74: "EncryptionItem",
    0x75: "AppSpecific", 0x76: "RepositoryRoot", 0x78: "AutomationParameters",
    0x79: "ControllerAssignments", 0x7A: "Module", 0x7B: "ModuleBank",
}

APP_IDS = {6: "Absynth", 7: "Massive"}   # Authoring application ids (item 0x65 data)

MAX_DEPTH = 32


class NISError(ValueError):
    pass


class Frame:
    def __init__(self, domain, item_id, version, data):
        self.domain, self.item_id, self.version, self.data = domain, item_id, version, data

    @property
    def name(self):
        if self.domain == DOMAIN_NISD:
            return ITEM_NAMES.get(self.item_id, f"Unknown_{self.item_id:#x}")
        return f"{self.domain[::-1].decode('latin-1')}_{self.item_id:#x}"


class Item:
    def __init__(self):
        self.version = 0
        self.flags = 0
        self.uuid = None
        self.frames: list[Frame] = []   # outermost (most derived) first
        self.children: list[Item] = []
        self.subtree: "Item | None" = None   # decoded SubtreeItem payload
        self.raw_header = b""                # bytes 8..40 of the header (version, magic, flags, reserved, uuid)
        self.child_keys: list[bytes] = []    # the 12-byte (index, domain, id) record before each child
        self.children_version = 1

    @property
    def type_name(self):
        return self.frames[0].name if self.frames else "?"

    def frame(self, item_id):
        for f in self.frames:
            if f.domain == DOMAIN_NISD and f.item_id == item_id:
                return f
        return None

    def walk(self, depth=0):
        yield depth, self
        if self.subtree is not None:
            yield from self.subtree.walk(depth + 1)
        for c in self.children:
            yield from c.walk(depth + 1)


def _need(buf, off, n):
    if off < 0 or off + n > len(buf):
        raise NISError(f"truncated at offset {off} (need {n} bytes, have {len(buf) - off})")


def _u32(buf, off):
    _need(buf, off, 4)
    return struct.unpack_from("<I", buf, off)[0]


def _u64(buf, off):
    _need(buf, off, 8)
    return struct.unpack_from("<Q", buf, off)[0]


def parse(buf: bytes, depth: int = 0) -> Item:
    if depth > MAX_DEPTH:
        raise NISError("nesting too deep")
    item, end = _parse_item(buf, 0, depth)
    return item


def _parse_item(buf, off, depth):
    length = _u64(buf, off)
    if length < 40 or off + length > len(buf):
        raise NISError(f"bad item length {length} at {off}")
    _need(buf, off, 40)
    version = _u32(buf, off + 8)
    if buf[off + 12:off + 16] != MAGIC:
        raise NISError(f"missing 'hsin' magic at {off + 12}")
    it = Item()
    it.version = version
    it.flags = _u32(buf, off + 16)
    it.uuid = str(_uuid.UUID(bytes_le=bytes(buf[off + 24:off + 40])))
    it.raw_header = bytes(buf[off + 8:off + 40])
    end = off + length
    p = off + 40
    p = _parse_frames(buf, p, end, it.frames, 0)
    # children
    cver = _u32(buf, p)
    count = _u32(buf, p + 4)
    p += 8
    if cver != 1:
        raise NISError(f"children list version {cver} at {p - 8}")
    if count > 10000:
        raise NISError("implausible child count")
    for _ in range(count):
        it.child_keys.append(bytes(buf[p:p + 12]))
        p += 12   # index, domain, item id (repeated in the child's own frames)
        child, p = _parse_item(buf, p, depth + 1)
        it.children.append(child)
    if p != end:
        raise NISError(f"item at {off}: {end - p} unread bytes")
    # SubtreeItem: decode its compressed payload as a nested item
    st = it.frame(0x73)
    if st is not None:
        it.subtree = parse(subtree_payload(st.data), depth + 1)
    return it, end


def _parse_frames(buf, p, limit, out, depth):
    if depth > MAX_DEPTH:
        raise NISError("frame stack too deep")
    size = _u64(buf, p)
    if size < 20 or p + size > limit:
        raise NISError(f"bad frame size {size} at {p}")
    domain = bytes(buf[p + 8:p + 12])
    item_id = _u32(buf, p + 12)
    version = _u32(buf, p + 16)
    end = p + size
    q = p + 20
    fr = Frame(domain, item_id, version, b"")
    out.append(fr)
    if not (domain == DOMAIN_NISD and item_id == 1):
        q = _parse_frames(buf, q, end, out, depth + 1)
    fr.data = bytes(buf[q:end])
    return end


def serialize(item: "Item") -> bytes:
    """Inverse of parse(). A decoded SubtreeItem is re-encoded (FastLZ literal runs, which
    every FastLZ decoder accepts); every other byte is written back as read."""
    from fastlz import compress_literal
    frames = item.frames
    datas = []
    for f in frames:
        d = f.data
        if f.domain == DOMAIN_NISD and f.item_id == 0x73 and item.subtree is not None:
            inner = serialize(item.subtree)
            stream = compress_literal(inner)
            d = struct.pack("<IB", 1, 1) + struct.pack("<II", len(inner), len(stream)) + stream
        datas.append(d)

    def frame(i):
        f = frames[i]
        inner = frame(i + 1) if not (f.domain == DOMAIN_NISD and f.item_id == 1) else b""
        body = f.domain + struct.pack("<II", f.item_id, f.version) + inner + datas[i]
        return struct.pack("<Q", 8 + len(body)) + body
    kids = struct.pack("<II", item.children_version, len(item.children))
    for key, c in zip(item.child_keys, item.children):
        kids += key + serialize(c)
    body = item.raw_header + frame(0) + kids
    return struct.pack("<Q", 8 + len(body)) + body


def subtree_payload(data: bytes) -> bytes:
    if len(data) < 5:
        raise NISError("SubtreeItem too short")
    ver = _u32(data, 0)
    if ver != 1:
        raise NISError(f"SubtreeItem version {ver}")
    compressed = data[4]
    if compressed:
        dsize = _u32(data, 5)
        csize = _u32(data, 9)
        _need(data, 13, csize)
        try:
            return decompress(data[13:13 + csize], dsize)
        except FastLZError as e:
            raise NISError(f"SubtreeItem: {e}") from e
    # uncompressed: the u64 at 5 is the inner item's own length field, so the slice starts there
    # (the item begins with its length; same reading as ni-file's SubtreeItem). Not a bug.
    size = _u64(data, 5)
    _need(data, 5, size)
    return bytes(data[5:5 + size])


def read_wstring(buf, off):
    """u32 count + count UTF-16LE code units. Returns (str, new offset)."""
    n = _u32(buf, off)
    if n > 1 << 16:
        raise NISError("string too long")
    _need(buf, off + 4, 2 * n)
    return bytes(buf[off + 4:off + 4 + 2 * n]).decode("utf-16-le", "replace"), off + 4 + 2 * n


# ----------------------------------------------------------------- writer ---
# Minimal writer, used only to build synthetic fixtures for the tests.

def build_item(frames, children=(), version=1, flags=1, item_uuid=b"\0" * 16):
    """frames: list of (item_id, version, data) outermost first; the last must be id 1."""
    def frame(i):
        iid, ver, data = frames[i]
        inner = frame(i + 1) if i + 1 < len(frames) else b""
        body = DOMAIN_NISD + struct.pack("<II", iid, ver) + inner + data
        return struct.pack("<Q", 8 + len(body)) + body
    assert frames[-1][0] == 1
    fr = frame(0)
    kids = struct.pack("<II", 1, len(children))
    for i, (cid, cbytes) in enumerate(children):
        kids += struct.pack("<I", i) + DOMAIN_NISD + struct.pack("<I", cid) + cbytes
    body = struct.pack("<I", version) + MAGIC + struct.pack("<II", flags, 0) + item_uuid + fr + kids
    return struct.pack("<Q", 8 + len(body)) + body


def wstring(s: str) -> bytes:
    b = s.encode("utf-16-le")
    return struct.pack("<I", len(b) // 2) + b
