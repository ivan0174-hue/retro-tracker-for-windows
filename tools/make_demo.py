#!/usr/bin/env python3
"""Generate demo8.mod: a short original 8-channel song with synthesized samples."""
import math, random, struct, sys

R = 16574  # C-3 sample rate
PT = [856,808,762,720,678,640,604,570,538,508,480,453,
      428,404,381,360,339,320,302,285,269,254,240,226,
      214,202,190,180,170,160,151,143,135,127,120,113]
NAMES = ['C-','C#','D-','D#','E-','F-','F#','G-','G#','A-','A#','B-']

def period(n):  # 'A-2'
    return PT[(int(n[2]) - 1) * 12 + NAMES.index(n[:2])]

def c8(v):
    return max(-128, min(127, int(round(v))))

def kick():
    out, ph = [], 0.0
    for i in range(3600):
        t = i / R
        ph += 2 * math.pi * (42 + 120 * math.exp(-t * 28)) / R
        out.append(c8(150 * math.exp(-t * 8) * math.sin(ph)))
    return out

def snare():
    random.seed(7)
    out = []
    for i in range(4200):
        t = i / R
        noise = random.uniform(-1, 1) * math.exp(-t * 16)
        tone = math.sin(2 * math.pi * 185 * t) * math.exp(-t * 35)
        out.append(c8(110 * (0.7 * noise + 0.6 * tone)))
    return out

def hihat():
    random.seed(3)
    out, prev = [], 0.0
    for i in range(1400):
        t = i / R
        x = random.uniform(-1, 1)
        out.append(c8(95 * (x - prev) * 0.5 * math.exp(-t * 55)))
        prev = x
    return out

def cycle(fn, n):
    return [c8(fn(i / n)) for i in range(n)]

samples = [
    ("kick", kick(), 0, 0, 64),
    ("snare", snare(), 0, 0, 56),
    ("hihat", hihat(), 0, 0, 40),
    ("bass (saw)", cycle(lambda p: 118 * (1 - 2 * p), 64), 0, 64, 64),
    ("lead (square)", cycle(lambda p: 80 if p < 0.5 else -80, 32), 0, 32, 40),
    ("pad (soft)", cycle(lambda p: 90 * math.sin(2 * math.pi * p) + 25 * math.sin(6 * math.pi * p), 64), 0, 64, 40),
    ("arp (pulse)", cycle(lambda p: 70 if p < 0.25 else -70, 32), 0, 32, 34),
]

ROWS, CH = 64, 8

def empty():
    return [[None] * CH for _ in range(ROWS)]

def put(p, r, ch, note=None, smp=0, cmd=0, par=0):
    p[r][ch] = (note, smp, cmd, par)

chords = [("A-1", "A-2", "C-3", "037"), ("F-1", "F-2", "A-2", "047"), ("C-2", "C-2", "E-2", "047"), ("G-1", "G-2", "B-2", "047")]

def drums(p, full=True):
    for r in range(0, 64, 4):
        if full or r % 16 == 0:
            put(p, r, 0, "C-3", 1)
    if full:
        for r in range(4, 64, 8):
            put(p, r, 1, "C-3", 2)
        put(p, 62, 1, "C-3", 2, 0xC, 0x20)
    for r in range(0, 64, 2):
        put(p, r, 2, "C-3", 3, 0xC, 0x28 if r % 4 else 0x18)

def bass(p):
    for k, (root, *_rest) in enumerate(chords):
        for j in range(0, 16, 2):
            n = root if j % 4 == 0 else root[:2] + str(int(root[2]) + 1)
            put(p, k * 16 + j, 3, n, 4)

def pads(p):
    for k, (_r, a, b, arp) in enumerate(chords):
        put(p, k * 16, 5, a, 6, 4, 0x24)
        put(p, k * 16, 6, b, 6, 4, 0x24)

def arps(p):
    for k, (_r, a, _b, arp) in enumerate(chords):
        for j in range(0, 16, 4):
            put(p, k * 16 + j, 7, a, 7, 0, int(arp, 16))
            put(p, k * 16 + j + 1, 7, None, 0, 0, int(arp, 16))
            put(p, k * 16 + j + 2, 7, None, 0, 0, int(arp, 16))
            put(p, k * 16 + j + 3, 7, None, 0, 0xA, 0x04)

melody1 = {0: "E-3", 4: "C-3", 8: "A-2", 12: "C-3", 16: "A-2", 20: "C-3", 24: "F-3", 28: "E-3",
           32: "E-3", 36: "D-3", 40: "C-3", 44: "G-2", 48: "B-2", 52: "D-3", 56: "G-3", 60: "D-3"}
melody2 = {0: "A-3", 6: "G-3", 8: "E-3", 12: "C-3", 16: "F-3", 22: "E-3", 24: "C-3", 28: "A-2",
           32: "G-2", 34: "C-3", 36: "E-3", 40: "G-3", 48: "G-3", 52: "F#3", 56: "D-3", 60: "B-2"}

def lead(p, mel):
    for r, n in mel.items():
        put(p, r, 4, n, 5, 4, 0x34)

pats = []
p0 = empty(); drums(p0, full=False); bass(p0); pads(p0); put(p0, 0, 0, "C-3", 1, 0xF, 6); pats.append(p0)
p1 = empty(); drums(p1); bass(p1); pads(p1); arps(p1); lead(p1, melody1); pats.append(p1)
p2 = empty(); drums(p2); bass(p2); pads(p2); arps(p2); lead(p2, melody2); pats.append(p2)
p3 = empty(); bass(p3); pads(p3); arps(p3); lead(p3, melody1)
put(p3, 0, 0, "C-3", 1); put(p3, 32, 0, "C-3", 1)
put(p3, 63, 2, None, 0, 0xD, 0x00)
pats.append(p3)
orders = [0, 1, 2, 3, 1, 2]

out = bytearray()
out += b"PT8 demo".ljust(20, b"\0")
for i in range(31):
    if i < len(samples):
        name, data, ls, ll, vol = samples[i]
        if len(data) % 2: data = data + [0]
        out += name.encode().ljust(22, b"\0")[:22]
        out += struct.pack(">HBBHH", len(data) // 2, 0, vol, ls // 2, (ll // 2) if ll else 1)
    else:
        out += b"\0" * 22 + struct.pack(">HBBHH", 0, 0, 0, 0, 1)
out += bytes([len(orders), 127]) + bytes(orders + [0] * (128 - len(orders))) + b"8CHN"
for p in pats:
    for r in range(ROWS):
        for ch in range(CH):
            cell = p[r][ch]
            if cell is None:
                out += b"\0\0\0\0"
                continue
            note, smp, cmd, par = cell
            per = period(note) if note else 0
            out += bytes([(smp & 0xF0) | (per >> 8), per & 0xFF, ((smp & 0x0F) << 4) | cmd, par])
for s in samples:
    data = s[1] + ([0] if len(s[1]) % 2 else [])
    out += bytes((v & 0xFF) for v in data)
open(sys.argv[1] if len(sys.argv) > 1 else "demo8.mod", "wb").write(out)
print("demo written,", len(out), "bytes")
