#!/usr/bin/env python3
"""Intel HEX -> compact binary (.pbin) and gzipped (.pbin.gz) for the jig.

.pbin: b"PICB", then per contiguous run [u32 address LE][u16 length LE][data],
then a u32 CRC32 (zlib) of everything before it. The jig reads .hex, .pbin and
gzip of either; a script names the file, the jig tells the format by content.

Refuses a HEX without CONFIG4L (0x300006) or with LVP (bit 2) off: the jig
programs over LVP, and firmware with LVP off is never flashed.

usage: hex2pbin.py firmware.hex   -> firmware.pbin, firmware.pbin.gz
"""
import gzip
import struct
import sys
import zlib


def load_hex(path):
    mem, base = {}, 0
    for n, line in enumerate(open(path), 1):
        line = line.strip()
        if not line.startswith(':'):
            continue
        b = bytes.fromhex(line[1:])
        if sum(b) & 0xFF:
            sys.exit(f'{path}:{n}: bad checksum')
        count, addr, rtype = b[0], b[1] << 8 | b[2], b[3]
        if rtype == 0:
            for i in range(count):
                mem[base + addr + i] = b[4 + i]
        elif rtype == 4:
            base = (b[4] << 8 | b[5]) << 16
        elif rtype != 1:
            sys.exit(f'{path}:{n}: unsupported record type {rtype:02X}')
    return mem


def pbin(mem):
    out = bytearray(b'PICB')
    addrs = sorted(mem)
    i = 0
    while i < len(addrs):
        start, j = addrs[i], i
        while j + 1 < len(addrs) and addrs[j + 1] == addrs[j] + 1 and j + 1 - i < 0xFFFF:
            j += 1
        out += struct.pack('<IH', start, j - i + 1) + bytes(mem[a] for a in addrs[i:j + 1])
        i = j + 1
    return bytes(out + struct.pack('<I', zlib.crc32(out)))


def main():
    if len(sys.argv) != 2 or not sys.argv[1].endswith('.hex'):
        sys.exit(__doc__.strip().splitlines()[-1])
    src = sys.argv[1]
    mem = load_hex(src)
    c4l = mem.get(0x300006)
    if c4l is None:
        sys.exit(f'{src}: no CONFIG4L (0x300006): LVP must be set ON in the firmware')
    if not c4l & 0x04:
        sys.exit(f'{src}: CONFIG4L = 0x{c4l:02X}, LVP is OFF: set "#pragma config LVP = ON" and rebuild')
    data = pbin(mem)
    stem = src[:-4]
    open(stem + '.pbin', 'wb').write(data)
    open(stem + '.pbin.gz', 'wb').write(gzip.compress(data, 9, mtime=0))
    print(f'{src}: {len(mem)} bytes, CONFIG4L 0x{c4l:02X} (LVP on) -> '
          f'{stem}.pbin {len(data)} B, {stem}.pbin.gz {len(gzip.compress(data, 9, mtime=0))} B')


if __name__ == '__main__':
    main()
