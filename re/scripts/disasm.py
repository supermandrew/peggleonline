"""Disassemble the game EXE at a virtual address.

usage: re/tools/pyenv/Scripts/python re/scripts/disasm.py <hex_va> [count] [--exe path]
"""
import os
import struct
import sys

import capstone

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_EXE = os.path.join(HERE, '..', 'bin', 'PeggleNights_inner.exe')


def load(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    osz = struct.unpack_from('<H', d, pe + 20)[0]
    base = struct.unpack_from('<I', d, pe + 24 + 28)[0]
    secs = [struct.unpack_from('<IIII', d, pe + 24 + osz + 40 * i + 8) for i in range(nsec)]
    return d, base, secs


def va_to_off(base, secs, va):
    rva = va - base
    for vs, sva, rs, rp in secs:
        if sva <= rva < sva + rs:
            return rva - sva + rp
    raise ValueError(f'{va:#x} not in file')


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    exe = DEFAULT_EXE
    if '--exe' in sys.argv:
        exe = sys.argv[sys.argv.index('--exe') + 1]
        args.remove(exe)
    va = int(args[0], 16)
    count = int(args[1]) if len(args) > 1 else 40
    d, base, secs = load(exe)
    off = va_to_off(base, secs, va)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for i, ins in enumerate(md.disasm(d[off:off + count * 16], va)):
        if i >= count:
            break
        print(f'{ins.address:08x}  {ins.bytes.hex():<20} {ins.mnemonic} {ins.op_str}')


if __name__ == '__main__':
    main()
