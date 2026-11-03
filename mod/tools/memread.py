"""Read memory of the running 32-bit game process (diagnostics). Usage: python memread.py <pid> <hexaddr> <len>"""
import ctypes, struct, sys
from ctypes import wintypes
k = ctypes.WinDLL('kernel32', use_last_error=True)
k.OpenProcess.restype = wintypes.HANDLE
k.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]


class Proc:
    def __init__(self, pid):
        self.h = k.OpenProcess(0x0010 | 0x0400, False, pid)  # VM_READ | QUERY_INFORMATION
        if not self.h:
            raise OSError(ctypes.get_last_error())

    def read(self, addr, n):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t()
        if not k.ReadProcessMemory(self.h, addr, buf, n, ctypes.byref(got)):
            raise OSError(f"read {addr:#x}: {ctypes.get_last_error()}")
        return buf.raw[:got.value]

    def u32(self, addr):
        return struct.unpack('<I', self.read(addr, 4))[0]


if __name__ == '__main__':
    p = Proc(int(sys.argv[1]))
    data = p.read(int(sys.argv[2], 16), int(sys.argv[3], 0))
    for i in range(0, len(data), 16):
        print(f"{int(sys.argv[2], 16) + i:08x}  " + ' '.join(f"{w:08x}" for w in struct.unpack_from('<4I', data.ljust(i + 16, b'\0'), i)))
