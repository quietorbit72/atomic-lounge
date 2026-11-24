# SPDX-License-Identifier: AGPL-3.0-only
# Vintage Chonk: FastLZ (levels 1 and 2) decoder and a literal-only encoder.
#
# Written from the public FastLZ format description (FastLZ by Ariya Hidayat,
# MIT licence; no code copied). Native Instruments uses FastLZ level 1 inside
# NI Sound (hsin) SubtreeItem frames (see docs/MASSIVE_PRESET_FORMAT.md).

class FastLZError(ValueError):
    pass


MAX_OUTPUT = 64 * 1024 * 1024   # cap for forged size fields


def decompress(data: bytes, expected_size: int | None = None) -> bytes:
    if not data:
        return b""
    level = (data[0] >> 5) + 1
    if level not in (1, 2):
        raise FastLZError(f"unknown FastLZ level {level}")
    limit = expected_size if expected_size is not None else MAX_OUTPUT
    if limit > MAX_OUTPUT:
        raise FastLZError("declared output too large")
    out = bytearray()
    ip = 0
    n = len(data)
    ctrl = data[ip] & 31
    ip += 1
    while True:
        if ctrl >= 32:
            length = (ctrl >> 5) - 1
            ofs = (ctrl & 31) << 8
            if level == 1:
                if length == 6:
                    length += _byte(data, ip); ip += 1
                code = _byte(data, ip); ip += 1
                ref = len(out) - ofs - 1 - code
            else:
                if length == 6:
                    while True:
                        c = _byte(data, ip); ip += 1
                        length += c
                        if c != 255:
                            break
                code = _byte(data, ip); ip += 1
                ref = len(out) - ofs - 1 - code
                if code == 255 and ofs == (31 << 8):
                    ofs = (_byte(data, ip) << 8) | _byte(data, ip + 1)
                    ip += 2
                    ref = len(out) - ofs - 8191 - 1
            length += 3
            if ref < 0:
                raise FastLZError("back reference before start of output")
            if len(out) + length > limit:
                raise FastLZError("output exceeds declared size")
            for _ in range(length):          # byte by byte: references may overlap
                out.append(out[ref]); ref += 1
        else:
            ctrl += 1
            if ip + ctrl > n:
                raise FastLZError("literal run past end of input")
            if len(out) + ctrl > limit:
                raise FastLZError("output exceeds declared size")
            out += data[ip:ip + ctrl]
            ip += ctrl
        if ip >= n:
            break
        ctrl = data[ip]; ip += 1
    if expected_size is not None and len(out) != expected_size:
        raise FastLZError(f"decompressed {len(out)} bytes, expected {expected_size}")
    return bytes(out)


def _byte(data: bytes, i: int) -> int:
    if i >= len(data):
        raise FastLZError("truncated FastLZ stream")
    return data[i]


def compress_literal(data: bytes) -> bytes:
    """A valid level-1 stream made only of literal runs (no matches).
    Used to build synthetic test fixtures; NI's own files use real matches."""
    out = bytearray()
    for i in range(0, len(data), 32):
        chunk = data[i:i + 32]
        out.append(len(chunk) - 1)
        out += chunk
    return bytes(out)
