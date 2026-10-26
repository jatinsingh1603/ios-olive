#!/usr/bin/env python3
"""Apple Device Tree for the OLIVE xnu on the Redmi 8 (olive, SDM439).

  olive/tools/mkadt_olive.py OUT.adt [--dump]
  (or tools/mkadt.py OUT.adt --olive with olive/tools/mkadt-olive.patch applied)

The /chosen identity, security and boot-policy properties are the taimen
tree's (tools/mkadt.py), so iOS userland sees the same iPhone X (D22AP).
The board nodes follow the onclite tree (crsvt tools/mkadt.py --onclite):
GICv2, ramoops console, eMMC, shadow panel canvas. Never a /sweet-ufs node:
olive has eMMC, and xnu 0406 keeps the UFS hub off on OLIVE. Values from the olive
live FDT (device/live.dts), device/iomem.txt and probe v1 (NOTES.md).

Env:
  OLIVE_BASE / OLIVE_MEM  xnu's single RAM range (default 0xa0000000 +
                          0x5f400000: everything above ramoops to 4 GiB
                          minus the 12 MiB panel canvas). Must lie inside one
                          free olive range (FREE below), end at or below 4 GiB
                          and be 2 MiB aligned. First boots (M2) use
                          OLIVE_MEM=0x40000000 (1 GiB); raise it at M3.
  OLIVE_NCPU              cpu nodes (default 8: 0x100-0x103 then 0x000-0x003).
  OLIVE_ADT_ARGS          /chosen sweet-boot-args (xnu 0309), as TAIMEN_ADT_ARGS.
  OLIVE_GAMUT=P3          keep the iPhone X's P3 /product gamut (default sRGB, as taimen).
"""
import os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '../../tools'))
import adt  # noqa: E402
import mkadt  # noqa: E402  (helpers + the D22AP /product source; build() is not used)
from mkadt import node, u32, u64, cstr, SOC_BASE, BOOT_MANIFEST_HASH  # noqa: E402

TIMEBASE = 19200000                     # CNTFRQ, probe v1 0x124F800

# Free physical RAM for xnu: Linux System RAM (iomem.txt:110-117) minus the
# DT reserved-memory carve-outs (no-map other_ext/modem/adsp_fw/wcnss_fw =
# 0x84a00000-0x8dc00000, splash 0x90000000+0x1400000, ramoops 0x9ff00000+
# 0x100000) and the Linux-reserved 0x8f000000-0x8fffffff of unknown owner.
# The Linux-reserved runs above 0xf1038000 are reusable CMA pools / memblock
# allocations of the running kernel, not firmware (inferred: every no-map
# region is below 0x8dc00000).
FREE = [
    (0x10000000, 0x84a00000),   # low: also holds LK's kernel (0x10080000), DTB (0x13400000), initrd (0x13600000)
    (0x86700000, 0x86800000),
    (0x8dc00000, 0x8f000000),
    (0x91400000, 0x9ff00000),
    (0xa0000000, 0x100000000),
]
LK_LOADS = (0x10000000, 0x18000000)     # shim + DTB + initrd (payload) land here: keep xnu out

PHYS_BASE, RAM_TOP = 0xa0000000, 0x100000000
PANEL_SIZE = 0xc00000                   # onclite: 1125x2436 BGRA shadow canvas (0107 SweetFramebuffer)

SPLASH = 0x90000000                     # cont_splash_mem reg <0x90000000 0x1400000>
FB, FB_W, FB_H, FB_STRIDE, FB_BPP = 0x90001000, 720, 1520, 2160, 24  # probe v1 MDP RGB pipe, SRC_FORMAT 0x2243F
FB_SIZE = (FB_STRIDE * FB_H + 0xfff) & ~0xfff  # 0x321900 rounded up to a page: 0x322000
SPLASH_SIZE = 0x1400000
assert SPLASH <= FB and FB + FB_SIZE <= SPLASH + SPLASH_SIZE, 'fbcon reg outside the LK splash region'
RAMOOPS, RAMOOPS_CONSOLE = 0x9ff00000, 0x80000  # = OLIVE.h SWEET_EARLY_LOG_PRZ / _SIZE
EARLY_LOG_BLOCK = 0x9fe00000            # OLIVE.h: the 2 MiB Device block start.s maps around it
GICD, GICD_SIZE = 0x0b000000, 0x1000    # qcom,msm-qgic2
GICC, GICC_SIZE = 0x0b002000, 0x1000
VTIMER_INTID = 20                       # probe v1: vtimer 20 / ptimer 19, 29 IRQs/300 ms each
EMMC = (0x07824900, 0x500, 0x07824000, 0x800)  # sdhc_1 hc_mem, core_mem (live.dts:3848)
OLIVE_ECID = 0x4f4c495645000001         # 'OLIVE' + 1, synthetic (not the phone's)

# (MPIDR, L2 cluster = Aff1, Hz). Linux CPU0-3 = 0x100-0x103 (capacity
# 0x466, the fast cluster, boot core first; live.dts cpu@100), CPU4-7 =
# 0x000-0x003 (capacity 0x400). 0x100 must stay first: SweetCPU (xnu 0404)
# panics unless logical CPU 0 is MPIDR 0x100 (OLIVE_BOOT_MPIDR), and cpus=1
# keeps only the 'running' node. Clocks are SDM439 datasheet maxima
# (inferred, not read from the phone).
CPUS = [(0x100, 1, 1958400000), (0x101, 1, 1958400000), (0x102, 1, 1958400000), (0x103, 1, 1958400000),
        (0x000, 0, 1459200000), (0x001, 0, 1459200000), (0x002, 0, 1459200000), (0x003, 0, 1459200000)]


def ram_window():
    base = int(os.environ['OLIVE_BASE'], 0) if os.environ.get('OLIVE_BASE') else PHYS_BASE
    end_free = next((hi for lo, hi in FREE if lo <= base < hi), None)
    assert end_free is not None, f'OLIVE_BASE {base:#x} is not in free olive RAM'
    default = end_free - base - PANEL_SIZE
    size = int(os.environ['OLIVE_MEM'], 0) if os.environ.get('OLIVE_MEM') else default
    assert base % 0x200000 == 0 and size % 0x200000 == 0 and size >= 0x10000000, (hex(base), hex(size))
    # xnu's arm64 RAM window and the shim's checks (SH-1e) are 32-bit physical
    # on olive: nothing above 4 GiB exists, and a size past it must fail here.
    assert base + size <= RAM_TOP, f'RAM {base:#x}+{size:#x} runs past 4 GiB'
    assert base + size + PANEL_SIZE <= end_free, \
        f'RAM {base:#x}+{size:#x} plus the panel canvas runs past free RAM at {end_free:#x}'
    assert base >= LK_LOADS[1] or base + size + PANEL_SIZE <= LK_LOADS[0], 'xnu RAM overlaps where LK loads the shim/DTB/initrd'
    # start.s (xnu 0402) maps the splash and the early-log block as V=P Device
    # 2 MiB blocks inside RAM's L1-index-2 L2 table; RAM must not cover them.
    for lo, n in ((SPLASH, SPLASH_SIZE), (EARLY_LOG_BLOCK, 0x200000)):
        assert base + size + PANEL_SIZE <= lo or base >= lo + n, \
            f'RAM {base:#x}+{size:#x} overlaps the early Device block {lo:#x}+{n:#x}'
    return base, size


def build(seed=None):
    phys_base, mem_size = ram_window()
    panel_base = phys_base + mem_size   # right above xnu's RAM, never handed to the VM
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    extra_args = os.environ.get('OLIVE_ADT_ARGS', '').strip()
    chosen = node('chosen', [ph()] + ([('sweet-boot-args', cstr(extra_args))] if extra_args else []) + [
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('firmware-version', cstr('olive-shim')),
        ('system-firmware-version', cstr('olive-shim')),
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        ('crypto-hash-method', cstr('sha2-384')),
        # 720x1520 panel presented as the iPhone X canvas (1125x2436 @3x), as onclite
        ('display-scale', u32(3)),
        ('unique-chip-id', struct.pack('<Q', OLIVE_ECID)),
        ('chip-id', u32(0x8015)),
        # synthetic, locally administered (not the phone's)
        ('mac-address-wifi0', bytes.fromhex('02005e77ef10')),
        ('mac-address-bluetooth0', bytes.fromhex('02005e77ef11')),
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # Boot core first (logical CPU 0 = MPIDR 0x100, 'running'), then the
    # rest of its cluster, then cluster 0. Homogeneous A53s: cluster-type 0
    # is CLUSTER_TYPE_SMP, the only type MAX_CPU_CLUSTERS 1 can hold ('E'/'P'
    # would split them); l2-cache-id (Aff1) tells the two L2s apart.
    # cluster-core-id is explicit because the SWEET default in
    # ml_parse_cpu_topology is phys_id >> 8, i.e. 1 for every boot-cluster
    # core. Compatible: SweetCPU's IONameMatch (KernelConfigTables, 0404)
    # matches any entry; 'qcom,kryo280' is kept in case the table loses
    # 'arm,cortex-a53' again.
    ncpu = int(os.environ.get('OLIVE_NCPU') or 8)
    assert 1 <= ncpu <= 8
    assert CPUS[0][0] == 0x100, 'the boot core (OLIVE_BOOT_MPIDR) must be logical CPU 0'
    cpu_nodes = []
    for core, (mpidr, cluster, hz) in enumerate(CPUS[:ncpu]):
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr('arm,cortex-a53') + cstr('ARM,v8') + cstr('qcom,kryo280')),
            ('reg', u32(mpidr)),
            ('cpu-id', u32(core)),
            ('cluster-id', u32(0)),
            ('cluster-type', u32(0)),
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(cluster)),
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(TIMEBASE)),
            ('clock-frequency', u32(hz)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(933000000)),
            ('peripheral-frequency', u32(TIMEBASE)),
            ('fixed-frequency', u32(TIMEBASE)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)), ('#size-cells', u32(0))], cpu_nodes)

    # GICv2 (xnu 0403 pe_fiq.c HAS_GIC_V2): /arm-io/gic, reg = <GICD size GICC
    # size> as four u64 (32 bytes, checked), arm-io relative (+ SOC_BASE).
    # virtual-timer-intid must be a PPI (16-31). No redistributor-stride: GICv2
    # has no redistributors.
    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-400')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(VTIMER_INTID)),
        ('reg', u64(GICD - SOC_BASE, GICD_SIZE, GICC - SOC_BASE, GICC_SIZE)),
    ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('sweet-io')),
        ('compatible', cstr('sweet-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic,
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('sweet,display')),
            ('canvas-width', u32(1125)),
            ('canvas-height', u32(2436)),
        ])])

    # Text console on the LK splash (pe_serial.c fbcon_probe, 24 bpp, xnu 0079):
    # LK's RGB0 pipe, which probe v2 saw scanning out after `fastboot boot`.
    # The byte order (B,G,R as the Pixel 1, or R,G,B) is still open; it is
    # the kernel's choice (fbcon_px, 0410), not the ADT's.
    # NO kickoff-reg, ever: the panel is video mode and the MDP scans memory
    # continuously; with kickoff-reg fbcon_probe would map CTL0/VIG0/DSI from
    # it and sweet_fbcon_kick would write CTL0 START and VIG0 into the running
    # video pipe.
    fbcon = node('sweet-fbcon', [
        ph(),
        ('reg', u64(FB, FB_SIZE)),
        ('width', u32(FB_W)),
        ('height', u32(FB_H)),
        ('stride', u32(FB_STRIDE)),
        ('bits-per-pixel', u32(FB_BPP)),
    ])
    # sweet_early.c console into the ramoops console zone. The kernel takes
    # the zone from OLIVE.h (SWEET_EARLY_LOG_PRZ 0x9ff00000, 0x80000, xnu 0402),
    # not from here; this node records it for tools and must agree.
    ramoops = node('sweet-ramoops', [ph(), ('reg', u64(RAMOOPS, RAMOOPS_CONSOLE))])
    # sdhc_1 for the M3 eMMC driver (0407). No /sweet-ufs node: 0406 refuses
    # the UFS hub on OLIVE before its ADT lookup, and must keep doing so.
    emmc = node('olive-emmc', [ph(), ('compatible', cstr('qcom,sdhci-msm')), ('reg', u64(*EMMC))])
    # 12 MiB right above xnu RAM, never handed to the VM. No kernel reads it
    # yet (0410 allocates its 1125x2436 shadow contiguously from the VM); kept
    # so the default RAM size stays 0x5f400000 and a fixed canvas stays possible.
    panel = node('olive-panel', [ph(), ('reg', u64(panel_base, PANEL_SIZE))])

    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])

    product_props = mkadt.d22ap_product(mkadt.D22AP_ADT)
    if product_props and os.environ.get('OLIVE_GAMUT', 'sRGB') != 'P3':
        product_props = [(k, cstr('sRGB') if k in ('artwork-display-gamut', 'display-gamut') else v)
                         for k, v in product_props]
    product = [node('product', [ph()] + product_props)] if product_props else []

    # compatible[0] = hw.model (D22AP), model = hw.machine (iPhone10,3).
    # 'xiaomi,sweet' keeps SweetPlatformExpert (xnu 0016) matching;
    # 'xiaomi,olive' is for OLIVE-only drivers (onclite 0101 matches 'xiaomi,onclite').
    return node('device-tree', [
        ph(),
        ('compatible', cstr('D22AP') + cstr('iPhone10,3') + cstr('xiaomi,sweet') + cstr('xiaomi,olive')
         + cstr('qcom,sdm439') + cstr('AppleARM')),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('D22')),
        ('target-sub-type', cstr('D22AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, ramoops, emmc, panel, buttons] + product)


def main(argv):
    root = build()
    blob = adt.serialize(root)
    open(argv[1], 'wb').write(blob)
    if '--dump' in argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{argv[1]}: {len(blob)} bytes', file=sys.stderr)


if __name__ == '__main__':
    main(sys.argv)
