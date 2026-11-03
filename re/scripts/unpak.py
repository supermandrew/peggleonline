"""Extract a PopCap .pak archive (XOR 0xF7, magic 0xBAC04AC0).

usage: python unpak.py <file.pak> <out_dir>
"""
import os
import struct
import sys


def unpak(pak_path, out_dir):
    raw = open(pak_path, 'rb').read()
    d = bytes(b ^ 0xF7 for b in raw)
    magic, version = struct.unpack_from('<II', d, 0)
    if magic != 0xBAC04AC0:
        raise SystemExit(f"bad magic {magic:#x}")
    pos, entries = 8, []
    while True:
        flags = d[pos]
        pos += 1
        if flags & 0x80:
            break
        nlen = d[pos]
        pos += 1
        name = d[pos:pos + nlen].decode('latin1')
        pos += nlen
        size, _ftime = struct.unpack_from('<iQ', d, pos)
        pos += 12
        entries.append((name, size))
    for name, size in entries:
        dst = os.path.join(out_dir, *name.replace('\\', '/').split('/'))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f:
            f.write(d[pos:pos + size])
        pos += size
    print(f"{pak_path}: version {version}, {len(entries)} files -> {out_dir}")


if __name__ == '__main__':
    unpak(sys.argv[1], sys.argv[2])
