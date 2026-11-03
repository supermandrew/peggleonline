"""Send a command to the mod's debug pipe in a running game. Usage: python dbg.py <pid> <command...>

  shot <file.png> [dd-offset-hex]   save the game's current frame (converted from BMP)
  ping
"""
import os
import sys
import tempfile
import time


def send(pid, line, timeout=15.0):
    name = rf'\\.\pipe\peggle-netplay-{pid}'
    deadline = time.time() + timeout
    while True:
        try:
            f = open(name, 'r+b', buffering=0)
            break
        except OSError:
            if time.time() > deadline:
                raise
            time.sleep(0.05)
    with f:
        f.write(line.encode() + b'\n')
        return f.read(65536).decode(errors='replace')


def main(argv):
    pid, cmd = int(argv[0]), argv[1:]
    if cmd[0] == 'shot':
        from PIL import Image
        out = os.path.abspath(cmd[1])
        tmp = os.path.join(tempfile.gettempdir(), f'peggle-shot-{pid}.bmp')
        reply = send(pid, f'shot {cmd[2] if len(cmd) > 2 else "0"} {tmp}')
        if reply == 'ok':
            Image.open(tmp).save(out)
            os.remove(tmp)
            reply = f'saved {out}'
        print(reply)
    else:
        print(send(pid, ' '.join(cmd)))


if __name__ == '__main__':
    main(sys.argv[1:])
