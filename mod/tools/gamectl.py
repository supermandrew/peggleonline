"""Drive running Peggle Nights windows for testing: list, screenshot, click, key, text.

  python gamectl.py list
  python gamectl.py shot <hwnd> <out.png>          # PrintWindow capture (works while covered)
  python gamectl.py sshot <hwnd> <out.png>         # copy from the screen (window must be uncovered)
  python gamectl.py click <hwnd> <x> <y> [right]    # client coordinates, posted messages
  python gamectl.py move <hwnd> <x> <y>
  python gamectl.py key <hwnd> <vk-hex>
  python gamectl.py text <hwnd>                     # window + child texts (message boxes)
  python gamectl.py place <hwnd> <x> <y>            # move window on screen
  python gamectl.py closeall                        # quit every running copy normally
"""
import ctypes
import sys
import time
from ctypes import wintypes

from PIL import Image

user32 = ctypes.WinDLL('user32', use_last_error=True)
gdi32 = ctypes.WinDLL('gdi32')
kernel32 = ctypes.WinDLL('kernel32')
user32.SetProcessDPIAware()

WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user32.EnumWindows.argtypes = [WNDENUMPROC, wintypes.LPARAM]
user32.EnumChildWindows.argtypes = [wintypes.HWND, WNDENUMPROC, wintypes.LPARAM]
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.GetForegroundWindow.restype = wintypes.HWND
user32.AttachThreadInput.argtypes = [wintypes.DWORD, wintypes.DWORD, wintypes.BOOL]
user32.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, wintypes.UINT]
user32.GetDC.argtypes = [wintypes.HWND]
user32.GetDC.restype = wintypes.HDC
user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
gdi32.CreateCompatibleDC.argtypes = [wintypes.HDC]
gdi32.CreateCompatibleDC.restype = wintypes.HDC
gdi32.CreateCompatibleBitmap.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
gdi32.CreateCompatibleBitmap.restype = wintypes.HBITMAP
gdi32.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
gdi32.SelectObject.restype = wintypes.HGDIOBJ
gdi32.GetDIBits.argtypes = [wintypes.HDC, wintypes.HBITMAP, wintypes.UINT, wintypes.UINT, ctypes.c_void_p,
                            ctypes.c_void_p, wintypes.UINT]
gdi32.DeleteObject.argtypes = [wintypes.HGDIOBJ]
gdi32.DeleteDC.argtypes = [wintypes.HDC]
gdi32.BitBlt.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.HDC,
                         ctypes.c_int, ctypes.c_int, wintypes.DWORD]
user32.ClientToScreen.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.POINT)]

WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP = 0x200, 0x201, 0x202
WM_RBUTTONDOWN, WM_RBUTTONUP, WM_KEYDOWN, WM_KEYUP = 0x204, 0x205, 0x100, 0x101


def process_name(pid):
    h = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return '?'
    buf = ctypes.create_unicode_buffer(520)
    n = wintypes.DWORD(520)
    kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n))
    kernel32.CloseHandle(h)
    return buf.value


def window_text(h):
    buf = ctypes.create_unicode_buffer(1024)
    user32.GetWindowTextW(h, buf, 1024)
    return buf.value


def class_name(h):
    buf = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(h, buf, 256)
    return buf.value


def game_windows():
    out = []

    def cb(h, _):
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
        path = process_name(pid.value)
        name = path.rsplit('\\', 1)[-1].lower()
        if (name.startswith('popcapgame') or name.startswith('pegglenights')) and user32.IsWindowVisible(h):
            r = wintypes.RECT()
            user32.GetClientRect(h, ctypes.byref(r))
            out.append((h, pid.value, name, class_name(h), window_text(h), r.right, r.bottom))
        return True

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return out


def activate(h):
    """Bring a game window to the front; the game ignores mouse input while it is inactive."""
    fg = user32.GetForegroundWindow()
    fg_thread = user32.GetWindowThreadProcessId(fg, None)
    me = kernel32.GetCurrentThreadId()
    user32.AttachThreadInput(me, fg_thread, True)
    user32.SetForegroundWindow(h)
    user32.BringWindowToTop(h)
    user32.AttachThreadInput(me, fg_thread, False)


def lparam(x, y):
    return (int(y) & 0xFFFF) << 16 | (int(x) & 0xFFFF)


def shot(h, out, screen=False):
    r = wintypes.RECT()
    user32.GetClientRect(h, ctypes.byref(r))
    w, ht = r.right, r.bottom
    hdc = user32.GetDC(0 if screen else h)
    mdc = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, ht)
    old = gdi32.SelectObject(mdc, bmp)
    if screen:  # copy what is on screen (window must be visible and uncovered)
        pt = wintypes.POINT(0, 0)
        user32.ClientToScreen(h, ctypes.byref(pt))
        gdi32.BitBlt(mdc, 0, 0, w, ht, hdc, pt.x, pt.y, 0x00CC0020)
    else:
        user32.PrintWindow(h, mdc, 3)  # PW_CLIENTONLY | PW_RENDERFULLCONTENT

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [('biSize', wintypes.DWORD), ('biWidth', ctypes.c_long), ('biHeight', ctypes.c_long),
                    ('biPlanes', wintypes.WORD), ('biBitCount', wintypes.WORD), ('biCompression', wintypes.DWORD),
                    ('biSizeImage', wintypes.DWORD), ('biXPelsPerMeter', ctypes.c_long),
                    ('biYPelsPerMeter', ctypes.c_long), ('biClrUsed', wintypes.DWORD),
                    ('biClrImportant', wintypes.DWORD)]

    bih = BITMAPINFOHEADER()
    bih.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bih.biWidth, bih.biHeight, bih.biPlanes, bih.biBitCount = w, -ht, 1, 32
    buf = ctypes.create_string_buffer(w * ht * 4)
    gdi32.GetDIBits(mdc, bmp, 0, ht, buf, ctypes.byref(bih), 0)
    gdi32.SelectObject(mdc, old)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mdc)
    user32.ReleaseDC(0 if screen else h, hdc)
    Image.frombuffer('RGBX', (w, ht), buf.raw, 'raw', 'BGRX', 0, 1).convert('RGB').save(out)
    print(f"saved {out} ({w}x{ht})")


def main(argv):
    cmd = argv[0] if argv else 'list'
    if cmd == 'closeall':  # ask every running copy to quit normally (WM_CLOSE)
        for w in game_windows():
            if w[3] == 'MainWindow':
                user32.PostMessageW(w[0], 0x0010, 0, 0)
        return
    if cmd == 'list':
        for h, pid, name, cls, title, w, ht in game_windows():
            print(f"hwnd={h} pid={pid} {name} class={cls!r} title={title!r} client={w}x{ht}")
        return
    h = int(argv[1], 0)
    if cmd in ('shot', 'sshot'):
        shot(h, argv[2], screen=cmd == 'sshot')
    elif cmd in ('click', 'move'):
        x, y = int(argv[2]), int(argv[3])
        if user32.GetForegroundWindow() != h:
            activate(h)
            time.sleep(0.15)
        right = len(argv) > 4 and argv[4] == 'right'
        user32.PostMessageW(h, WM_MOUSEMOVE, 0, lparam(x, y))
        if cmd == 'click':
            down, up, flag = (WM_RBUTTONDOWN, WM_RBUTTONUP, 2) if right else (WM_LBUTTONDOWN, WM_LBUTTONUP, 1)
            user32.PostMessageW(h, down, flag, lparam(x, y))
            user32.PostMessageW(h, up, 0, lparam(x, y))
    elif cmd == 'key':
        vk = int(argv[2], 16)
        user32.PostMessageW(h, WM_KEYDOWN, vk, 1)
        user32.PostMessageW(h, WM_KEYUP, vk, 0xC0000001)
    elif cmd == 'text':
        print(f"[{class_name(h)}] {window_text(h)!r}")

        def cb(c, _):
            print(f"  [{class_name(c)}] {window_text(c)!r}")
            return True

        user32.EnumChildWindows(h, WNDENUMPROC(cb), 0)
    elif cmd == 'place':
        user32.SetWindowPos(h, 0, int(argv[2]), int(argv[3]), 0, 0, 0x0001 | 0x0004 | 0x0010)
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
