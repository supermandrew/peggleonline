"""Checks the built mod for floats kept in xmm registers across a call into the game.

The compiler takes an __asm block to leave the xmm registers alone, so where a block that calls the game is inlined
into a function holding floats in them, it keeps them there across the call. The game's code may change them (its
drawing goes through Direct3D and the video driver, its sounds through BASS), and the float comes back as garbage: a red
brick's picture was drawn at random places this way, and inside the simulation it would make the two computers differ.
Functions with such blocks are GAME_CALL (never inlined), and this finds any place where that was missed: after a call
through a register or memory (how __asm calls the game), an xmm register read before anything writes it.

  python mod\\tools\\xmmcheck.py [path\\to\\bass.dll]    (default: mod\\build\\bass.dll; needs dumpbin, from the build tools)

Prints each such call with the function it's in and exits with 1 if there are any.
"""
import glob
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DUMPBIN_GLOB = r'C:\Program Files*\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx86\x86\dumpbin.exe'

# A destination xmm register these only write (the rest of the instructions with one read it too).
WRITE_ONLY = re.compile(r'^(mov|cvt|sqrt|rcp|rsqrt|pshuf)')
COMPARE = {'comiss', 'ucomiss', 'comisd', 'ucomisd', 'ptest'}  # (read both, write flags)
ZEROING = {'xorps', 'xorpd', 'pxor', 'subps', 'subpd', 'psubd'}  # with itself: writes zero, reads nothing
STOP = {'ret', 'jmp', 'call', 'int3'}


def xmm(op):
    return set(re.findall(r'\bxmm[0-7]\b', op))


def access(mnemonic, operands):
    """The xmm registers an instruction reads and writes."""
    if not operands:
        return set(), set()
    dst, srcs = operands[0], operands[1:]
    reads = set().union(*(xmm(s) for s in srcs)) if srcs else set()
    writes = set()
    if re.fullmatch(r'xmm[0-7]', dst):
        if mnemonic in COMPARE:
            reads.add(dst)
        elif mnemonic in ZEROING and srcs == [dst]:
            writes.add(dst)
            reads.discard(dst)
        else:
            writes.add(dst)
            if not WRITE_ONLY.match(mnemonic):
                reads.add(dst)
    return reads, writes


def main(argv):
    dll = argv[0] if argv else os.path.join(HERE, '..', 'build', 'bass.dll')
    dumpbin = shutil.which('dumpbin') or (sorted(glob.glob(DUMPBIN_GLOB)) or [None])[-1]
    if not dumpbin:
        raise SystemExit('dumpbin.exe not found (it comes with the Visual Studio build tools)')
    listing = subprocess.run([dumpbin, '/disasm:nobytes', dll], capture_output=True, text=True,
                             errors='replace').stdout
    code = []  # (function, mnemonic, operands)
    function = '?'
    for line in listing.splitlines():
        label = re.match(r'^(\S.*):\s*$', line)
        if label:
            function = label.group(1)
            continue
        m = re.match(r'^\s+[0-9A-F]{8}:\s+(\S+)\s*(.*)$', line)
        if m:
            code.append((function, m.group(1), [o.strip() for o in m.group(2).split(',')] if m.group(2) else []))
    bad = []
    for i, (function, mnemonic, operands) in enumerate(code):
        if mnemonic != 'call' or not operands:
            continue
        target = operands[0]
        if not (re.fullmatch(r'e(ax|bx|cx|dx|si|di|bp)', target) or 'ptr' in target) or '__imp_' in target:
            continue  # (a direct call, or one to a DLL's import: compiled code, which keeps the calling convention)
        written = set()
        for f, m, ops in code[i + 1:i + 80]:
            if m in STOP or f != function:
                break
            reads, writes = access(m, ops)
            if reads - written:
                bad.append((function, target, m + ' ' + ','.join(ops)))
                break
            written |= writes
    for function, target, use in bad:
        print(f'call {target}, then {use}  in {function}')
    print(f'{len(bad)} calls with a float kept in an xmm register across them')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
