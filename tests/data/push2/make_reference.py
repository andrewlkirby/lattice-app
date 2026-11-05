"""Reference bytes for Push 2's display (tests/control/PushTests.cpp), written straight from
Ableton's "Push 2 MIDI and Display Interface Manual" (github.com/Ableton/push-interface), section
"Display Interface Protocol", and independent of Lattice's encoder:

- 160 lines of 960 pixels, top line first, leftmost pixel first;
- each line 2048 bytes: 1920 bytes of pixels, then 128 filler bytes;
- a pixel is 16 bits, least significant byte first: blue in bits 15-11, green 10-5, red 4-0;
- every line buffer XORed with 0xFFE7F3E7, little-endian (bytes E7 F3 E7 FF, repeating).

The test pattern: red rises left to right, green top to bottom, blue is (x + y) & 255. Prints the
64-bit FNV-1a hash of the 327,680 bytes after the 16-byte header (and their SHA-256, to compare
with by hand); the test checks Lattice's encoder gives the same. Run: python make_reference.py
"""
import hashlib

WIDTH, HEIGHT, LINE = 960, 160, 2048
PATTERN = bytes([0xE7, 0xF3, 0xE7, 0xFF])


def colour(x, y):
    return (x * 255 // (WIDTH - 1), y * 255 // (HEIGHT - 1), (x + y) & 255)


def frame():
    out = bytearray()
    for y in range(HEIGHT):
        line = bytearray(LINE)
        for x in range(WIDTH):
            r, g, b = colour(x, y)
            value = ((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3)
            line[2 * x] = value & 0xFF
            line[2 * x + 1] = value >> 8
        out += bytes(v ^ PATTERN[i % 4] for i, v in enumerate(line))
    return bytes(out)


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


if __name__ == "__main__":
    data = frame()
    assert len(data) == 327680
    print("fnv1a64: 0x%016x" % fnv1a64(data))
    print("sha256:", hashlib.sha256(data).hexdigest())
    print("first 8 bytes:", data[:8].hex(" "))
    print("line 0 filler starts:", data[1920:1924].hex(" "))
