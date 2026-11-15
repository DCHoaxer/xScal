#!/usr/bin/env python3
"""Rewrites 32-bit lossless bitmaps of a SWF as 8-bit colormapped bitmaps.

mxmlc stores every embedded PNG as DefineBitsLossless2 format 5: one ARGB word per pixel, zlib
compressed. A picture with at most 256 distinct ARGB values fits format 3 instead: a palette of
RGBA entries and one byte per pixel, about a third of the size for the credits pictures. The palette
keeps the premultiplied values mxmlc wrote, so both formats give the same colors. Bitmaps with more
than 256 values, and bitmaps that would not get smaller, are left as they are.

Usage: index_bitmaps.py <file.swf>

The file is rewritten in place and keeps its compression (none or zlib).
"""
import struct
import sys
import zlib

DEFINE_BITS_LOSSLESS2 = 36
FORMAT_COLORMAPPED = 3
FORMAT_ARGB = 5


def read_tags(body, start):
    """Yields (code, header and payload bytes, payload bytes) for each tag from start."""
    pos = start
    while pos < len(body):
        code_and_length = struct.unpack_from("<H", body, pos)[0]
        code = code_and_length >> 6
        length = code_and_length & 0x3F
        header_size = 2
        if length == 0x3F:
            length = struct.unpack_from("<I", body, pos + 2)[0]
            header_size = 6
        end = pos + header_size + length
        yield code, body[pos:end], body[pos + header_size:end]
        pos = end
        if code == 0:
            break


def write_tag(code, payload):
    # The long form is valid for every tag and is required above 62 bytes.
    return struct.pack("<HI", (code << 6) | 0x3F, len(payload)) + payload


def index_bitmap(payload):
    """Returns the format 3 payload for a format 5 DefineBitsLossless2 payload, or None."""
    character_id, bitmap_format, width, height = struct.unpack_from("<HBHH", payload, 0)
    if bitmap_format != FORMAT_ARGB:
        return None
    pixels = zlib.decompress(payload[7:])
    if len(pixels) != width * height * 4:
        return None
    palette = {}
    indexes = bytearray()
    row_padding = (-width) % 4
    for y in range(height):
        row = pixels[y * width * 4:(y + 1) * width * 4]
        for x in range(0, width * 4, 4):
            argb = row[x:x + 4]
            index = palette.get(argb)
            if index is None:
                if len(palette) == 256:
                    return None
                index = len(palette)
                palette[argb] = index
            indexes.append(index)
        # Colormapped rows are padded to a multiple of 4 bytes.
        indexes.extend(b"\0" * row_padding)
    table = bytearray()
    for argb in palette:
        table += bytes((argb[1], argb[2], argb[3], argb[0]))
    data = zlib.compress(bytes(table) + bytes(indexes), 9)
    header = struct.pack("<HBHHB", character_id, FORMAT_COLORMAPPED, width, height, len(palette) - 1)
    result = header + data
    return result if len(result) < len(payload) else None


def main():
    path = sys.argv[1]
    with open(path, "rb") as file:
        swf = file.read()
    signature = swf[:3]
    if signature == b"CWS":
        body = zlib.decompress(swf[8:])
    elif signature == b"FWS":
        body = swf[8:]
    else:
        sys.exit(f"{path}: not an uncompressed or zlib compressed SWF")
    # Frame size RECT: 5 bits of field size, then four fields; then frame rate and frame count.
    rect_bits = 5 + 4 * (body[0] >> 3)
    tags_start = (rect_bits + 7) // 8 + 4
    out = bytearray(body[:tags_start])
    count = 0
    saved = 0
    for code, raw, payload in read_tags(body, tags_start):
        indexed = index_bitmap(payload) if code == DEFINE_BITS_LOSSLESS2 else None
        if indexed is None:
            out += raw
            continue
        out += write_tag(code, indexed)
        count += 1
        saved += len(payload) - len(indexed)
    header_rest = swf[3:4] + struct.pack("<I", 8 + len(out))
    if signature == b"CWS":
        result = signature + header_rest + zlib.compress(bytes(out), 9)
    else:
        result = signature + header_rest + bytes(out)
    with open(path, "wb") as file:
        file.write(result)
    print(f"index_bitmaps: {count} bitmaps stored as 8-bit indexes, {saved} bytes smaller before SWF compression")


if __name__ == "__main__":
    main()
