#!/usr/bin/env python3
"""Host-side control client for rkkvm-hid (RK3506 IP-KVM HID daemon).

Usage:
  kvmctl.py type "Hello world!"
  kvmctl.py key 0x04            # tap HID usage code
  kvmctl.py kd 0x02 0x04        # key down (mod, key)
  kvmctl.py ku 0x02 0x04        # key up
  kvmctl.py move 20 -10
  kvmctl.py click left|right|middle
  kvmctl.py raw "m 5 5 1 0"
"""
import socket
import sys
import time

HOST = "192.168.0.103"
PORT = 5001

# US layout: char -> (hid usage code, needs shift)
_BASE = {
    'a': 0x04, 'b': 0x05, 'c': 0x06, 'd': 0x07, 'e': 0x08, 'f': 0x09,
    'g': 0x0A, 'h': 0x0B, 'i': 0x0C, 'j': 0x0D, 'k': 0x0E, 'l': 0x0F,
    'm': 0x10, 'n': 0x11, 'o': 0x12, 'p': 0x13, 'q': 0x14, 'r': 0x15,
    's': 0x16, 't': 0x17, 'u': 0x18, 'v': 0x19, 'w': 0x1A, 'x': 0x1B,
    'y': 0x1C, 'z': 0x1D,
    '1': 0x1E, '2': 0x1F, '3': 0x20, '4': 0x21, '5': 0x22, '6': 0x23,
    '7': 0x24, '8': 0x25, '9': 0x26, '0': 0x27,
    '\n': 0x28, '\x1b': 0x29, '\b': 0x2A, '\t': 0x2B, ' ': 0x2C,
    '-': 0x2D, '=': 0x2E, '[': 0x2F, ']': 0x30, '\\': 0x31, ';': 0x33,
    "'": 0x34, '`': 0x35, ',': 0x36, '.': 0x37, '/': 0x38,
}
_SHIFT = {
    'A': 'a', 'B': 'b', 'C': 'c', 'D': 'd', 'E': 'e', 'F': 'f', 'G': 'g',
    'H': 'h', 'I': 'i', 'J': 'j', 'K': 'k', 'L': 'l', 'M': 'm', 'N': 'n',
    'O': 'o', 'P': 'p', 'Q': 'q', 'R': 'r', 'S': 's', 'T': 't', 'U': 'u',
    'V': 'v', 'W': 'w', 'X': 'x', 'Y': 'y', 'Z': 'z',
    '!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7',
    '*': '8', '(': '9', ')': '0', '_': '-', '+': '=', '{': '[', '}': ']',
    '|': '\\', ':': ';', '"': "'", '~': '`', '<': ',', '>': '.', '?': '/',
}
LSHIFT = 0x02


class Client:
    def __init__(self, host=HOST, port=PORT):
        self.s = socket.create_connection((host, port), 5)
        self.buf = b""

    def cmd(self, line):
        self.s.sendall((line + "\n").encode())
        while b"\n" not in self.buf:
            d = self.s.recv(64)
            if not d:
                raise ConnectionError("closed")
            self.buf += d
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode().strip()

    def tap(self, code, mod=0):
        self.cmd(f"k {mod:x} {code:x}")

    def type(self, text, delay=0.008):
        for ch in text:
            mod = 0
            if ch in _SHIFT:
                mod = LSHIFT
                ch = _SHIFT[ch]
            code = _BASE.get(ch)
            if code is None:
                continue
            self.tap(code, mod)
            time.sleep(delay)

    def move(self, dx, dy, btn=0, wheel=0):
        self.cmd(f"m {dx} {dy} {btn} {wheel}")

    def click(self, button="left"):
        b = {"left": 1, "right": 2, "middle": 4}.get(button, 1)
        self.move(0, 0, b, 0)
        time.sleep(0.03)
        self.move(0, 0, 0, 0)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    c = Client()
    op = sys.argv[1]
    if op == "type":
        c.type(" ".join(sys.argv[2:]))
    elif op == "key":
        c.tap(int(sys.argv[2], 0))
    elif op == "kd":
        print(c.cmd(f"kd {int(sys.argv[2],0):x} {int(sys.argv[3],0):x}"))
    elif op == "ku":
        print(c.cmd(f"ku {int(sys.argv[2],0):x} {int(sys.argv[3],0):x}"))
    elif op == "move":
        c.move(int(sys.argv[2]), int(sys.argv[3]))
    elif op == "click":
        c.click(sys.argv[2] if len(sys.argv) > 2 else "left")
    elif op == "raw":
        print(c.cmd(" ".join(sys.argv[2:])))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
