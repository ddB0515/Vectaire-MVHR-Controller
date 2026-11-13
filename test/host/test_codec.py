#!/usr/bin/env python3
"""Cross-check the firmware's vbus_codec.c against the reference tool vectaire_bus.py.

The reference tool was verified byte-for-byte against the original panel firmware (emulated),
so agreement here means the ESP32 speaks exactly the panel's framing.
Usage: test_codec.py <codec_cli binary> <path to vectaire_bus.py>
"""
import importlib.util
import random
import subprocess
import sys

cli, ref_path = sys.argv[1], sys.argv[2]
spec = importlib.util.spec_from_file_location('vb', ref_path)
vb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vb)


def run(*args):
    return subprocess.run([cli, *args], capture_output=True, text=True, check=True).stdout.split('\n')


rng = random.Random(1)
cases = [(0xD0, 0x00, b''), (0xD0, 0x02, b''), (0xD0, 0x0E, b'\x01'), (0xD0, 0x11, b'\xff'),
         (0xD1, 0x0F, bytes([0x31, 1, 3]) + bytes(46))]
cases += [(rng.choice((0xD0, 0xD1)), rng.randrange(0x18), bytes(rng.randrange(256) for _ in range(rng.randrange(60))))
          for _ in range(2000)]
fail = 0
for addr, cmd, data in cases:
    ref = vb.build_frame(cmd, data, addr)
    ours = bytes.fromhex(run('enc', f'{addr:02x}', f'{cmd:02x}', *[f'{b:02x}' for b in data])[0])
    if ours != ref:
        fail += 1
        print('ENCODE MISMATCH', hex(cmd), data.hex(), ours.hex(), ref.hex())
        continue
    dec = run('dec', *[f'{b:02x}' for b in ref])[0].split()
    if dec[:3] != ['FRAME', f'{addr:02x}', f'{cmd:02x}'] or bytes.fromhex(''.join(dec[3:])) != data:
        fail += 1
        print('DECODE MISMATCH', hex(cmd), data.hex(), dec)

# corruption must be rejected: flip one body byte
bad = 0
for addr, cmd, data in cases[:300]:
    w = bytearray(vb.build_frame(cmd, data, addr))
    i = rng.randrange(1, len(w) - 1)
    w[i] ^= 1 << rng.randrange(8)
    if any(l.startswith('FRAME') for l in run('dec', *[f'{b:02x}' for b in w])):
        bad += 1
print(f'{len(cases)} frames: {fail} mismatches; {bad}/300 corrupted frames wrongly accepted')
sys.exit(1 if fail or bad else 0)
