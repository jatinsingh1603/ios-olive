#!/usr/bin/env python3
"""SWEETXNU payload (tools/mkpayload.py layout) around the olive shim test stub.

  mkstubpayload.py STUB.bin STUB_VMSIZE ADT OUT [--pad MB] [--extra "boot-args"]

Laid out like a 4K-page TAIMEN/OLIVE kernel from mkpayload.py --l2block
0x200000: virt_base 2 MB aligned, entry at virt_base + 0x4000.
  seg 0  the stub (vmsize covers its .bss + stack: the shim zero-fills it)
  seg 1  zero-fill check: 8 file bytes "LLIFOREZ", vmsize 64 KB
  seg 2  --pad MB filler (default 16; onclite LK failed at an 84 MB image)
The command line carries what the stub must find (see stub.c).
"""
import argparse, os, struct, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../tools'))
import mkpayload  # noqa: E402

VIRT_BASE = 0xfffffff007000000	# TAIMEN __TEXT 0xfffffff007004000 & ~(2 MB - 1)
PAGE = 0x4000
M64 = (1 << 64) - 1


def sum64(data):
    """FNV-1a over little-endian 8-byte words, zero-padded tail (olive/shim/util.h)."""
    data = bytes(data) + b'\0' * (-len(data) % 8)
    h = 0xcbf29ce484222325
    for (w,) in struct.iter_unpack('<Q', data):
        h = ((h ^ w) * 0x100000001b3) & M64
    return h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('stub'); ap.add_argument('vmsize', type=lambda v: int(v, 0))
    ap.add_argument('adt'); ap.add_argument('out')
    ap.add_argument('--pad', type=int, default=16)
    ap.add_argument('--extra', default='', help='more boot-args, e.g. olive_gicq=0 olive_hold=1')
    a = ap.parse_args()

    stub = open(a.stub, 'rb').read()
    adt_blob = open(a.adt, 'rb').read()
    phys_base, mem_size = mkpayload.adt_dram(adt_blob)
    entry = VIRT_BASE + PAGE
    z_va, z_len, z_data = VIRT_BASE + 0x100000, 0x10000, b'LLIFOREZ'
    pad_va = VIRT_BASE + 0x1000000
    pad = bytes(range(256)) * (a.pad << 12)
    assert a.vmsize >= len(stub) and entry + a.vmsize <= z_va
    pieces = [(entry, (a.vmsize + PAGE - 1) & ~(PAGE - 1), stub), (z_va, z_len, z_data), (pad_va, len(pad), pad)]
    kernel_end = (pad_va + len(pad) + PAGE - 1) & ~(PAGE - 1)

    out = bytearray(mkpayload.SEG_TABLE + 32 * len(pieces))
    table = []
    for va, size, data in pieces:
        off = len(out)
        out += data
        out += b'\0' * (-len(out) & 0xf)
        table.append((va, size, off, len(data)))
    adt_off = len(out)
    out += adt_blob
    out += b'\0' * (-len(out) & 0xf)
    struct.pack_into('<8sII', out, 0, b'SWEETXNU', 1, len(pieces))
    struct.pack_into('<7Q', out, 0x10, VIRT_BASE, entry, adt_off, len(adt_blob), kernel_end, phys_base, mem_size)
    cl = (f'olive-stub pb={phys_base:#x} ms={mem_size:#x} vb={VIRT_BASE:#x} '
          f'adtlen={len(adt_blob):#x} adtsum={sum64(adt_blob):#x} '
          f'padva={pad_va:#x} padlen={len(pad):#x} padsum={sum64(pad):#x} '
          f'zva={z_va:#x} zlen={z_len:#x} {a.extra}').strip().encode()
    assert len(cl) < mkpayload.BOOT_LINE_LENGTH, len(cl)
    out[mkpayload.CMDLINE:mkpayload.CMDLINE + len(cl)] = cl
    for i, t in enumerate(table):
        struct.pack_into('<4Q', out, mkpayload.SEG_TABLE + 32 * i, *t)
    open(a.out, 'wb').write(out)
    print(f'{a.out}: {len(out)} bytes, entry pa {entry - VIRT_BASE + phys_base:#x}, '
          f'phys {phys_base:#x}+{mem_size:#x}, cmdline {len(cl)} bytes')


if __name__ == '__main__':
    main()
