# OLIVE port plan (merged, reviewed)

This merges the six reviewed lanes (gating, gic-cpu, memory-console, storage, display, shim-tools) into one ordered plan for the OLIVE port. New xnu patches 0400-0413 go into ~/ios-olive/repo/xnu/patches and apply after taimen's 0336. Four existing staging scripts are edited in place, and there are shim, ADT and CI changes.

**Conflicts I settled, with the reason for each:**

1. **OLIVE is SWEET + OLIVE, not TAIMEN.** The display lane (D1) wanted OLIVE to inherit ARM64_BOARD_CONFIG_TAIMEN. Four lanes said no, and their evidence holds:
   - start.s:563-575 writes to 0x9d400000 with the MMU off, which is ordinary RAM on olive.
   - start.s:726-770 would turn xnu RAM 0xa0000000-0xb01fffff into Device memory.
   - TAIMEN.h:24-25 puts the early log at 0xb0000000, inside olive's xnu RAM.
   - sweet_early.c:26 sets a 1 MiB PRZ cap, but olive's zone is 0x80000.
   - SweetPlatformExpert.cpp:51-364 holds the taimen WDT/DSI/GICv3 dumps.
   - I checked SweetFramebuffer.cpp myself. The scaled path at :638-711 is plain SWEET, so the display work still applies. The TAIMEN blocks (:377-480 shot, :960-985 presenter cpu-7 binding, :1112-1353 hw-scale) compile out, which is what we want. So display-lane D6 (a)/(c) needs no edit, only the #else review.
2. **OLIVE.h is layered after SWEET.h.** It uses #undef/#define, the same way TAIMEN.h:12-25 already does (verified). The gating lane wanted to replace SWEET.h instead; layering duplicates less. GICD_CTLR and GICD_CTLR_ENABLEGRP1 must not be redefined, because the build uses -Werror.
3. **The generic TAIMEN gates are widened with an explicit `|| defined(ARM64_BOARD_CONFIG_OLIVE)`.** These sites test command-line macros, so a header macro would be unsafe at vm_param.h, zalloc.c and ubc_subr.c.
4. **xnu RAM is 0xa0000000 + 0x5f400000.** That is the shim-tools default. mkadt_olive.py:82-83 already puts the panel canvas and DSI page at phys_base+mem_size = 0xff400000. The memory lane's 0x60000000 would overlap that.
   - For the first boot use OLIVE_MEM=0x40000000 (arm_init.c:402: exactly 1 GiB keeps 4K VM pages). The panel then sits at 0xe0000000.
   - Either way, everything at 0x90000000-0x9fffffff stays below RAM inside L1 index 2, so the start.s path that reuses RAM's existing L2 table stays valid.
5. **The PRZ header is protected on both sides.** The stage-0 append at start.s:537-560 has no bounds check and runs with the MMU off. The shim already writes a valid DBGC header (repo/olive/shim/plog.h); the kernel also gets a signature and size check.
6. **Restart:** PS_HOLD, plus PON warm reset, plus SOFT_RB_SPARE, following onclite 0105.
   - Probe v1 is confounded: probe.c:277-287 calls PSCI SYSTEM_RESET before it writes PS_HOLD.
   - The shim pre-set becomes MANDATORY before any iOS root is written to userdata (M3). For M2, booting into Lineage is an acceptable way to read pstore.
7. **The GICv2 SPI API (sweet_spi_attach/mask/dispatch) is mandatory for the link.** Onclite 0101 does not have it.
8. **The SweetCPU.cpp:299 boot-MPIDR panic is an M2 blocker.** It fires on every boot, cpus=1 included.

**What already exists:**
- repo/olive/shim: QEMU stub run PASSES. This is the M1 base.
- repo/olive/tools/mkadt_olive.py.

**Not checked in this merge:** that the new patches apply. Run apply-check.sh after every patch, because CI skips failing patches silently.



## P00 Build/CI scaffolding: staging-script regexes, xnu.yml, apply-check gate [M2-REQUIRED]
files: repo/xnu/patches/0019-stage-libpthread.sh, repo/xnu/patches/0020-stage-bearssl.sh, repo/xnu/patches/0026-stage-hfs.sh, repo/xnu/patches/0175-stage-ionetworking.sh, repo/.github/workflows/xnu.yml, tools/apply-check.sh
risk: Low. A missed regex usually breaks the build loudly. A stale firehose or fakeroot cache is hard to diagnose.
refs: 0019:5, 0020:4, 0026:6, 0175:8, xnu.yml:12-13,22,39,62-65,117,125-127, dxb/build.sh:584,606-622

Not a new patch. Edit in place in ~/ios-olive/repo/xnu/patches:
- 0019-stage-libpthread.sh:5, 0020-stage-bearssl.sh:4, 0026-stage-hfs.sh:6, 0175-stage-ionetworking.sh:8: change `^(SWEET|TAIMEN)$` to `^(SWEET|TAIMEN|OLIVE)$`. The MACOS 13.5 guards at 0019:6, 0026:7 and 0175:9 stay as they are.

.github/workflows/xnu.yml:
- :22: add `olive` to branches.
- :12-13 and :39: default machine_config to OLIVE on the olive branch.
- :62-65: parameterise the libfirehose -mcpu sed and its grep -c assertion with FIREHOSE_MCPU (cortex-a53 for OLIVE).
- :117: change the fakeroot key to `fakeroot-${CACHE_SALT}-fh-${FIREHOSE_MCPU}-${MACHINE_CONFIG}-...`. Reason: dxb build.sh:584 installs headers with TARGET_CONFIGS including MACHINE_CONFIG, and :606 only rebuilds firehose when the library is missing.
- The ccache key (:125-127) already contains MACHINE_CONFIG.

Gate after every new patch: `~/ios-olive/tools/apply-check.sh ~/ios-olive/repo/xnu/patches OLIVE ~/ios-olive/build/xnu-OLIVE`.
- There must be no SCRIPT FAIL and no SKIP beyond the 4 known dxb ones.
- apply-check does not report staging scripts that exit 0, so also check that these directories exist in xnu-OLIVE: bsd/sweet_hfs/core, bsd/pthread/sweet_libpthread, libkern/crypto/sweet_bearssl, iokit/IOKit/network.


## P0400 0400-olive-machine-config.patch: MACHINE_CONFIG=OLIVE, OLIVE.h, MASTER [M2-REQUIRED]
files: build/xnu-TAIMEN/makedefs/MakeInc.def, build/xnu-TAIMEN/bsd/conf/Makefile.template, build/xnu-TAIMEN/libkern/conf/Makefile.template, build/xnu-TAIMEN/config/MASTER.arm64.olive (new), build/xnu-TAIMEN/pexpert/pexpert/arm64/board_config.h, build/xnu-TAIMEN/pexpert/pexpert/arm64/OLIVE.h (new), build/xnu-TAIMEN/pexpert/pexpert/arm64/Makefile
risk: Low, but there are two silent failures: a missing MASTER.arm64.olive (no HFS) and a missed :347 (the 4K kernel is linked at the LARGE address).
refs: MakeInc.def:32,64-69,138,269,345-351, board_config.h:100-112, TAIMEN.h:12-25, SWEET.h:26,36,43-46,57,65,68, bsd/conf/Makefile:20-21, live.dts:83-89,4218

makedefs/MakeInc.def:
- :32: add OLIVE to SUPPORTED_ARM64_MACHINE_CONFIGS.
- After :69 add `MACHINE_FLAGS_ARM64_OLIVE = -DARM64_BOARD_CONFIG_SWEET -DARM64_BOARD_CONFIG_OLIVE -mcpu=cortex-a53 -mfix-cortex-a53-835769`. Do NOT define TAIMEN. Use -mcpu, not -march, so LSE stays off (comment at :64-65).
- :269: the filter becomes `SWEET TAIMEN OLIVE`. Otherwise OLIVE builds as arm64e (PAC) on an A53.
- :347: becomes `ifneq ($(filter $(CURRENT_MACHINE_CONFIG),TAIMEN OLIVE),)`. This gives -UARM_LARGE_MEMORY and the 0xfffffff007004000 link address.

Other build files:
- bsd/conf/Makefile.template:48 and libkern/conf/Makefile.template:22: add OLIVE to the `SWEET TAIMEN` filter. These carry the hfs compat -include and the pthread/BearSSL include flags.
- New config/MASTER.arm64.olive: a copy of MASTER.arm64.taimen (:34 FILESYS_BASE includes sweet_hfs). If it is missing, the build silently falls back to MASTER and the kernel has no HFS.
- pexpert/pexpert/arm64/Makefile:21-22: export OLIVE.h.
- board_config.h:101-104: after the TAIMEN include add `#ifdef ARM64_BOARD_CONFIG_OLIVE` / `#include <pexpert/arm64/OLIVE.h>` / `#endif`. MAX_CPUS 8 and MAX_CPU_CLUSTERS 1 are unchanged.

New pexpert/pexpert/arm64/OLIVE.h, layered after SWEET.h:
- `#define OLIVE 1`
- `#undef __ARM_16K_PG__` (MMFR0 0x1122)
- `#undef HAS_GIC_V3` and `#define HAS_GIC_V2 1`
- `#undef SWEET_VTIMER_INTID` and `#define SWEET_VTIMER_INTID 20` (live.dts:4218: PPI4 = CNTV)
- `#define OLIVE_BOOT_MPIDR 0x100`
- `#undef/#define SWEET_EARLY_LOG_BLOCK 0x9fe00000`, `SWEET_EARLY_LOG_PRZ 0x9ff00000`, `#define SWEET_EARLY_LOG_PRZ_SIZE 0x80000` (live.dts:83-89)
- Early-screen and alias constants: OLIVE_SPLASH_PA 0x90000000, OLIVE_SPLASH_BLOCKS 10, OLIVE_FB_PA 0x90001000, OLIVE_FB_W 720, OLIVE_FB_H 1520, OLIVE_FB_STRIDE 2160, OLIVE_PSHOLD_PA 0x004ab000
- GICC scalars: GICC_CTLR 0x0, GICC_PMR 0x4, GICC_BPR 0x8, GICC_IAR 0xC, GICC_EOIR 0x10, GICC_IAR_INTID_MASK 0x3ff. GICD scalars: GICD_TYPER 0x4, GICD_SGIR 0xF00, GICD_SGIR_TARGET_SHIFT 16.
- Do NOT redefine GICD_CTLR or GICD_CTLR_ENABLEGRP1 (SWEET.h:65/68; -Werror, MakeInc.def:138).
- Keep SWEET_IPI_SGI 1, GIC_SPURIOUS_IRQ 1023 and SWEET_SMP_MAX_CPUS 8.
- Do NOT define SWEET_IMEM_BASE here: sweet_perf.c:38-42 defines it unconditionally (see 0405).

The output kernel is kernel.development.olive (dxb build.sh:122).


## P0401 0401-olive-4k-vm-gates.patch: widen the generic 4K TAIMEN gates [M2-REQUIRED]
files: osfmk/arm/pmap/pmap.h, osfmk/mach/arm/vm_param.h, osfmk/kern/zalloc.c, osfmk/vm/vm_compressor.c, osfmk/vm/vm_shared_region.c, bsd/kern/ubc_subr.c, osfmk/arm64/arm_vm_init.c, osfmk/arm/pmap/pmap.c, osfmk/arm/cpuid.c
risk: High if a site is missed: nothing warns at compile time.
refs: pmap.h:105-121, vm_param.h:136-140, zalloc.c:163-172, vm_compressor.c:872, vm_shared_region.c:813, ubc_subr.c:375,5927, arm_vm_init.c:2366, pmap.c:2554-2571,10918, cpuid.c:391-398

At each site, add `|| defined(ARM64_BOARD_CONFIG_OLIVE)`. For the negative gate, change it to `&& !defined(ARM64_BOARD_CONFIG_OLIVE)`.
- osfmk/arm/pmap/pmap.h:114: BOOTSTRAP_TABLE_SIZE 20. This one is load-bearing: olive needs 7-8 pages and the default is 8, so an overflow corrupts memory silently with no log.
- osfmk/mach/arm/vm_param.h:136: negative gate. Without it MACH_VM_MAX_ADDRESS breaks under 39-bit TTBR0.
- osfmk/kern/zalloc.c:163
- osfmk/vm/vm_compressor.c:872 (0250: 4 x max_mem)
- osfmk/vm/vm_shared_region.c:813
- bsd/kern/ubc_subr.c:375 and :5927 (16K-signed dyld cache on a 4K kernel; needed for launchd at M3, harmless at M2)
- osfmk/arm64/arm_vm_init.c:2366: uses the header macro `defined(TAIMEN)`, so add `|| defined(OLIVE)`
- osfmk/arm/pmap/pmap.c:2554: region_select 4 for the early alias window. Without it, taimen boot 12 hit an L2 block-demotion panic.
- osfmk/arm/cpuid.c:391: c_bulksize_op=0, needed before SMP

Also check that 0254 (pmap.c:10918, no board guard) is present in xnu-OLIVE.

Validation: preprocess vm_param.h, zalloc.c, ubc_subr.c, pmap.h, vm_compressor.c and vm_shared_region.c with the TAIMEN flags and with the OLIVE flags. The 4K values must come out identical.


## P0402 0402-olive-early-map-and-log.patch: start.s, arm_vm_init.c and sweet_early.c 4K OLIVE variant [M2-REQUIRED]
files: osfmk/arm64/start.s, osfmk/arm64/arm_vm_init.c, osfmk/arm64/sweet_early.c, osfmk/arm64/sweet_early.h
risk: High. This code runs with the MMU off or before the console, and a mistake means silent death. The band and PRZ stage marks are the only signal; the panel stays lit after `fastboot boot` (probe v2).
refs: start.s:330-334,537-575,726-792, arm_vm_init.c:1828-1891,2003, sweet_early.c:24-30,34-99,110-127,197,263-279, NOTES.md:23-29, live.dts:78-89, iomem.txt:117-119

These three files share one layout and change together. Without this patch, the SWEET #else paths that compile into OLIVE are broken on 4K pages.

start.s:
(1) Stage-0 PRZ append at :537-560 (MMU off). Under OLIVE, before the store loop: if sig != 0x43474244 or size >= 0x80000-12-64, reset the header the way sweet_log_raw does (sweet_early.c:110-117).
(2) White band at :563-575. Add `#elif defined(OLIVE)`: x0 = 0x90001000 + 1400*2160 = 0x902e3380, 64 rows, 17280 8-byte stores of -1, no kick.
(3) Device V=P at :726-792. Restructure as `#if defined(TAIMEN)` / `#elif defined(OLIVE)` / `#elif defined(SWEET)`, so OLIVE never reaches the 16K 32 MB-block path at :776-792.
   - OLIVE: 10 blocks from 0x90000000 (splash) and 1 block at 0x9fe00000 (log), both inserted into RAM's existing L1-index-2 L2 table (the :738-748 branch).
   - Each run gets its own ATTRINDX(CACHE_ATTRINDX_DISABLE) loop (pattern at :750-760).
   - Do NOT map 0x91400000-0x9fe00000 (RAM in Linux) or 0xc800000.
   - Optional: a 0x00400000 IO run for PS_HOLD/MDP through create_bootstrap_mapping. If you add it, the attribute fix must loop over every new entry from x2, not patch one entry like taimen's :766-775.
(4) Wrap the ICC_SRE_EL2 code at :330-334 in `#if HAS_GIC_V3`. It is dead on olive (EL1 entry) but must stay ICC-free.

arm_vm_init.c:
- :1835-1891: change `#if defined(TAIMEN)` to `#if defined(TAIMEN) || defined(OLIVE)`, so OLIVE avoids the `#else /* !TAIMEN */` alias at VM_MIN_KERNEL_ADDRESS+2MB, which sits on an empty TTE in the 4K layout.
- OLIVE slots: 0-9 = 0x90000000 + i*2M, 10 = SWEET_EARLY_LOG_BLOCK, 11 = 0x00400000 (PS_HOLD block, for an early reset); no 0xc800000. SWEET_EARLY_ALIAS_BLOCKS = 12.
- sweet_early_map_final uses OLIVE_SPLASH_PA, not the hard-coded 0x9d400000 at :1866.
- :2003: no '+1 page' for OLIVE.

sweet_early.c:
- :26-30: PRZ_DATA_MAX = SWEET_EARLY_LOG_PRZ_SIZE-12 = 0x7fff4 for OLIVE. It must never be the 1 MiB TAIMEN value: that overruns pmsg, and Linux then drops the record.
- :34-99: an OLIVE block with no taimen_kick, early_kick or early_intr, and a bytewise 3 B/px sweet_early_band. Rows 1000+8*(stage%60), 6 rows each, using aligned 32-bit stores across the 2160-byte row (Device memory, no unaligned accesses). The 32-bit TAIMEN_FB_W writer would overrun the row.
- :197: mark call site.
- :267-275 relocate: early_fb = base + 0x1000; early_base = base + 10*2M; olive_early_pshold = base + 11*2M + 0xab000.
- sweet_early.h:10: add OLIVE.

Validation: grep the OLIVE preprocessed start.s, sweet_early.c and arm_vm_init.c for 0x9d400000, 0xc800000, 0xc90201c and `2 * PGBYTES`. None may remain.


## P0403 0403-olive-gicv2.patch: pe_fiq GICv2 branch with the SPI API, protos.h, sleh.c [M2-REQUIRED]
files: pexpert/arm/pe_fiq.c, pexpert/pexpert/arm/protos.h, osfmk/arm64/sleh.c
risk: High: this blocks both the link and the timer IRQ. The core logic is runtime-proven on SDM632 with the same IIDRs. The SPI functions are new code.
refs: pe_fiq.c:17,23,119-193,434-439, protos.h:38-41, sleh.c:55-57,1944-2000, sailfish_wlan.c:20,3181-3254, sweet_touch.c:143,581, bsd/conf/files:112,118, onclite 0101:188-341, live.dts:9683-9690, probe-v1.txt (typer 0x4EA iidr 0x1070)

pexpert/arm/pe_fiq.c: insert `#elif HAS_GIC_V2` before the empty #else at :434. Hand-port it from onclite 0101:188-341; 0101 does not apply cleanly.

On the boot core:
- Read /arm-io/gic reg = <0x0a000000,0x1000,0x0a002000,0x1000> + pe_arm_get_soc_base_phys(), giving 0x0b000000 / 0x0b002000.
- Read virtual-timer-intid.

On every core:
- `me = ml_get_cpu_number_local(); if (me >= MAX_CPUS) panic("OLIVE GIC: MPIDR 0x%llx not in ADT")`. Without this, an ADT mistake writes out of bounds of gic_cpu_iface_mask (machine_routines.c:1208-1246).
- Banked ITARGETSR0 byte gives this core's interface bit; it must be a single bit.
- IPRIORITYR 0-31 = 0xa0.
- ICENABLER0 = all except the vtimer and SGI1; ISENABLER0 = vtimer | SGI1.
- GICC_PMR 0xff, BPR 0, GICC_CTLR |= 1, GICD_CTLR |= GICD_CTLR_ENABLEGRP1, then dsb sy and isb.
- On the first pass only: ICENABLER and ICPENDR for SPIs 32..nirqs. nirqs = 32*((TYPER&0x1f)+1) = 352.
- Log `OLIVE GIC: GICv2 typer=%x iidr=%x iface=%x vtimer=%u`.

Define locally (function-like): GICD_ISENABLER(n) 0x100+4n, ICENABLER(n) 0x180+4n, ICPENDR(n) 0x280+4n, IPRIORITYR(n) 0x400+n, ITARGETSR(n) 0x800+n, ICFGR(n) 0xC00+4n.

Helpers:
- sweet_vtimer_intid
- sweet_gic_ack, sweet_gic_eoi (writes the full IAR)
- sweet_gic_send_sgi (dsb ishst; SGIR = mask<<16 | id)
- sweet_gic_cpu_ready

NEW and required for the link (sleh.c:1945/1989; sweet_touch.c:143,581; sailfish_wlan.c:3181-3254 are compiled `standard`):
- sweet_spi_attach: bounds checks (intid 32..nirqs-1, fn != NULL, gicd mapped), then ICENABLER, store fn/arg, dmb, ICFGR edge, IPRIORITYR 0xa0, ITARGETSR = this core's mask, ISENABLER. Read ISENABLER back and return false if the bit did not latch (Group-0 SPIs are RAZ/WI from the non-secure side).
- sweet_spi_mask.
- sweet_spi_dispatch (intid < 1020).

pexpert/pexpert/arm/protos.h:38-41: declare the gic_* prototypes (and optionally the spi_* ones) under `#if defined(ARM64_BOARD_CONFIG_OLIVE)`. Do NOT gate them on HAS_GIC_V2: sleh.c includes protos.h at :55, before board_config.h at :57, so the result would be an implicit declaration, which -Werror rejects.

osfmk/arm64/sleh.c:1957-2000, by hand:
- `#if HAS_GIC_V2`: iar = sweet_gic_ack(), intid = iar & 0x3ff, `#define SWEET_GIC_EOI(i) sweet_gic_eoi(i)`.
- `#else`: the ICC_IAR1/EOIR1 path.
- Replace the EOIR writes at :1980, :1984 and :1997 with the macro, and add `#undef SWEET_GIC_EOI`.
- Keep the taimen sweet_irq_seen kprintf, sweet_prof_irq, the 1020-1023 early return (no EOI) and sweet_spi_dispatch.


## P0404 0404-olive-cpu-topology.patch: SweetCPU boot MPIDR 0x100, GICv2 SGI, CPU match [M2-REQUIRED]
files: iokit/Kernel/arm/SweetCPU.cpp, iokit/KernelConfigTables.cpp
risk: Without (a), the first boot is a guaranteed panic. Low once (a) and (b) are in.
refs: SweetCPU.cpp:228,236,264-270,299-301,325-326,386,401-420,427,443-449, cpu.c:973-982, KernelConfigTables.cpp:41,47, onclite 0101:54-57,68-76,101-102

iokit/Kernel/arm/SweetCPU.cpp:
(a) :299-301: under OLIVE, panic only if `boot_cpu->cpu_id != 0 || boot_cpu->phys_id != OLIVE_BOOT_MPIDR` (onclite 0101:54-57). This is CRITICAL: startAll runs for the boot CPU at any cpus= value (:264-270), so the current check panics on every olive boot.
(b) :418: under HAS_GIC_V2, signalCPU calls sweet_gic_send_sgi(mask, SWEET_IPI_SGI) and keeps the trailing isb. Even at cpus=1 this is required: initCPU (:386) self-signals SGI1, and startAll waits up to 5 s (cpu.c:975-982).
(c) :427: replace `cpu->phys_id == 0` with `cpu == ml_get_topology_info()->boot_cpu` (0101:101-102). On olive, 0x000 is logical 4.
(d) :325-326 wait loop: under HAS_GIC_V2 add `!sweet_gic_cpu_ready(i) ||`.
(e) :236 cpuName: 'Cortex-A53' under OLIVE (cosmetic).

Do NOT take 0101:27-33 (gold-only check). SweetCPU.cpp:228 already accepts 0x000-0x103.

iokit/KernelConfigTables.cpp:47: append 'arm,cortex-a53' to the SweetCPU IONameMatch tuple, unconditionally (matching is additive). Do not take 0101:110-130, which renames the tuple to onclite.


## P0405 0405-olive-platform-reset.patch: halt_restart via PON warm + SOFT_RB_SPARE + PS_HOLD; IMEM [M2: PS_HOLD hook recommended; PON required before M3]
files: iokit/Kernel/arm/SweetPlatformExpert.cpp, bsd/dev/sweet_perf.c
risk: Medium-high. A reset that lands in Lineage after the iOS root is flashed can format userdata.
refs: SweetPlatformExpert.cpp:252,336-410, sweet_perf.c:36-42,1625-1680,1704-1766, live.dts:1690-1695,1717-1748,1983-1991,2235, onclite 0105:25-55,120-145,229-272, crsvt NOTES.md:165-180

iokit/Kernel/arm/SweetPlatformExpert.cpp: add an OLIVE block in start() (the whole TAIMEN block at :14-364 and :396-406 compiles out).
- ml_io_map PS_HOLD 0x004ab000 (live.dts:1690) in start(), not in panic context.
- Map SPMI: core 0x0200f000, chnls 0x02400000, obsrvr 0x02c00000, cnfg 0x0200a000, EE 0 (live.dts:1983-1991). Find the PON APID by scanning for PPID 0x008 and checking that its owner is EE 0.
- olive_halt_restart (port onclite 0105; filter kPEPanicSync/Begin/End/DiagnosticsDone, 0105:232-235):
  1. PON PS_HOLD_RESET_CTL 0x85a = (x&~0xf)|1 (warm), as required by the DT's force-warm-reboot;
  2. SOFT_RB_SPARE 0x88f = (x&3)|(mode<<2), then read it back;
  3. IMEM restart_reason 0x0860065c (harmless);
  4. PS_HOLD = 0.
- Boot-args olive_reset_to (default recovery) and olive_reboot_after.
- Early PE_halt_restart before IOKit can only drop PS_HOLD through the olive_early_pshold alias (0402). That is why the shim pre-set (SH-1b) is mandatory.

bsd/dev/sweet_perf.c:
- :38-42: add `#elif defined(ARM64_BOARD_CONFIG_OLIVE)` SWEET_IMEM_BASE 0x08600000.
- The PS_HOLD #else at :1721 is already 0x004ab000.
- Under OLIVE, skip the negative-guard perf_clocks() at :1629 and :1678 (MSM8996 clock MMIO). Note: perf_thread starts only from sweet_perf_init (:1737-1766), which only the hub calls, so it is dormant unless the OLIVE hub calls it (it must not).

Do not port taimen_wdt_* at :336-362. The olive WDT is handled in 0411.


## P0406 0406-olive-hub-and-rootfs-guards.patch: block the Pixel-1 driver hub; rootfix/mkdir/log-dedupe guards [M2: hub block; rest M3]
files: bsd/dev/sweet_ufs.c, bsd/kern/taimen_rootfix.c, bsd/kern/bsd_init.c, bsd/vfs/vfs_syscalls.c, bsd/dev/mem.c
risk: High if the hub block is missed: Pixel-1 drivers corrupt olive GPIO and clock state.
refs: sweet_ufs.c:675-689,745-785,938-950, sweet_touch.c:40-43,514-518,581,620-640, taimen_rootfix.c:18,594-656, bsd_init.c:1104-1108, vfs_syscalls.c:14583-14587, mem.c:207,362

bsd/dev/sweet_ufs.c:
- sweet_ufs_init (:676): under OLIVE, return ENXIO first, before any ADT lookup. Otherwise a /sweet-ufs node copied from the taimen ADT template would reach the #else hub at :776-783: sweet_keys_init at SPMI 0x0400f000, and sweet_touch_init, which is enabled by default and writes TLMM 0x01010000+pin*0x1000 (real olive GPIOs). It would also start sweet_usb_probe (DWC3), sweet_perf_init, sweet_gpu_init and sailfish_wlan_init.
- Also add `#elif defined(ARM64_BOARD_CONFIG_OLIVE)` with an empty list to the driver list at :749/:776.

For M3, change these guards to (TAIMEN || OLIVE), all together:
- bsd/kern/taimen_rootfix.c:18
- bsd/kern/bsd_init.c:1104
- bsd/vfs/vfs_syscalls.c:14583 (taimen_mkdir)
- bsd/dev/mem.c:207/362: /dev/null log de-duplication; this matters more with olive's 512K ring.

Optional, cosmetic: IOKitBSDInit.cpp:658, bsd_init.c:479, vm_resident.c:1220/1600.

The sched_prim.c taimen AMP code stays TAIMEN-only.


## P0407 0407-olive-emmc-root.patch: bsd/dev/olive_emmc.c from onclite 0106, with fixes [M3-REQUIRED]
files: bsd/dev/olive_emmc.c (new), bsd/conf/files, iokit/bsddev/IOKitBSDInit.cpp, refs/crsvt_xnu-android-port/xnu/patches/0106-onclite-emmc-root.patch, refs/console-ramoops_ios-santoni/NOTES.md
risk: Medium. Over-clocking or a descriptor-format mismatch causes silent CRC or command timeouts. Check the emmctest SHA-256 against Linux.
refs: 0106:55-57,417-443,567-806,832-910,927-1023,1097-1110,1139-1265, IOKitBSDInit.cpp:773-791, bsd/conf/files:107, live.dts:3847-3890, gpt.txt (userdata p62 start 17301504), santoni NOTES.md:3-35

New file bsd/dev/olive_emmc.c, from 0106:16-1237.
- Gate it on `#if defined(ARM64_BOARD_CONFIG_OLIVE)` (the compiler flag) and use the log prefix OLIVE EMMC.
- ADT node /olive-emmc, reg <0x07824900 0x500 0x07824000 0x800> (live.dts:3848).
- bsd/conf/files: add `bsd/dev/olive_emmc.c standard` after :107 (sweet_ufs.c). Do not take 0106's files hunk.
- IOKitBSDInit.cpp: an emmcroot0 branch under ARM64_BOARD_CONFIG_OLIVE, after the ufsroot0 block (:773-790) and before md (:791). On ECANCELED (emmctest), log and fall through to md0, or panic deliberately. Prefer fall-through, so the self-test output also reaches ramoops cleanly.
- Board hook (replaces 0106:1213-1219): sweet_sample_start(), olive_log_start() (0408), then olive_board_start() (empty until M5). Never sweet_usb_probe: olive has ChipIdea, not DWC3 (live.dts:13491).
- Boot-args emmcroot=userdata, emmcrw=1, emmctest=1, emmcrca=2, and the new emmcfirst=0x1080000, which panics on a GPT mismatch before writes are enabled.

Fix before the first root boot (santoni dff9f18): the raw cdev read/write stop using physio. Use a per-call kmem bounce, do the I/O under the lock, and uiomove outside it. In strategy, B_PHYS with a non-kernel VA uses copyin/copyout. Santoni saw PID 1 read /dev/remmcroot0 and panic in _memmove.

Takeover hardening for SDM439:
- log and clear HOST_CONTROL2 bits 12/13 (V4/64-bit; the DT has qcom,large-address-bus, live.dts:3850);
- log CQCFG at 0x7824e08 and clear its enable;
- log core+0x50 and core+0x78, VENDOR_SPEC hc+0x10c, and GCC sdcc1 RCGR 0x01842004-0x18 (read-only), to derive the real MCLK;
- caps bit 26 must be set before PWR_180.

Keep the 100 us register-write delay below 1 MHz and accept R1b busy-end. Bus speed stays at legacy or DDR52; no HS200/HS400.


## P0408 0408-olive-emmc-log-and-partapi.patch: route sweet_ufs part API and log ring to eMMC [M3, optional for first root boot]
files: bsd/dev/sweet_ufs.c, repo/tools/sweet-log-pull.sh
risk: Medium. Reusing the log thread naively gives a NULL-based memcpy panic; a forwarder placed after the su.ready check silently returns ENODEV.
refs: sweet_ufs.c:381-445,787-933, taimen_rootfix.c:501-560, gpt.txt

In sweet_ufs.c, under OLIVE:

Partition API:
- Forward sweet_ufs_part_find/part_read to olive_emmc_part_find/read as the FIRST statement of each, before the !su.ready checks at :393 and :419.
- Set *lun=0.
- Do bounds checks in 512-byte units and copy through the eMMC bounce in chunks of at most BOUNCE_SIZE.

Log ring: a new olive_log_thread, a copy of :868-917 with these differences:
- bounce = olive_emmc_bounce(), never su.dma+DMA_BOUNCE (su.dma is 0 on olive);
- olive_emmc_lock instead of sweet_ufs_lock;
- base_sect = ((nblocks/8)-1025)*8, header at 8 sectors, then blocks at base_sect+8+blk*8;
- olive_emmc_flush;
- tsleep on &sweet_log_chan.

olive_log_start runs only if emmcrw is set and nblocks >= 2*1025*8.

For olive the ring sits at partition-relative sector 104832976 (absolute LBA 0x7479FD0). This matches sweet-log-pull.sh:10-12. The HFS volume must stay at or below 53,674,483,712 B.


## P0409 0409-olive-lse-emulation.patch: onclite 0103 re-gated to OLIVE [optional safety net, M3]
files: osfmk/arm64/olive_lse.c (new), osfmk/arm64/sleh.c, osfmk/arm64/copyio.c, osfmk/arm64/machine_routines_asm.s, osfmk/conf/files.arm64
risk: Low
refs: onclite 0103:9-70,83-164,190-440, sleh.c:883-892

0103 applies cleanly to xnu-TAIMEN, but every hunk is gated on ONCLITE.
- Re-gate on ARM64_BOARD_CONFIG_OLIVE and rename onclite_lse.c to olive_lse.c.
- Delete onclite_diag_print and its onclite_emmc_stats extern (0103 lines 373-409).
- Keep the counter and the rate-limited log, and expect zero hits: taimen's iOS 16 userland reached the home screen on an LSE-less Kryo 280.
- Hold it back if the first M3 boot shows no SIGILL. Including it is cheap.


## P0410 0410-olive-display-clcd.patch: AppleCLCD 24bpp video-mode on the SWEET scaled path [M4-REQUIRED]
files: iokit/Kernel/arm/SweetFramebuffer.cpp, pexpert/arm/pe_serial.c, iokit/Kernel/arm/hid/SweetHID.cpp
risk: Medium. The 11 MB contiguous allocation may fail, and the byte order is still unverified.
refs: SweetFramebuffer.cpp:79,88-93,494-530,556-562,612-629,638-711,739-772,1040-1067,1355-1392,1514-1528,1651, pe_serial.c:723-735,1137-1183,1287-1342, serial_console.c:276-289, SweetHID.cpp:364-375,636-692

SweetFramebuffer.cpp, editing the SWEET (non-TAIMEN) path:
- startScaled (:640-660) under OLIVE uses the iPhone X canvas SWEET_FB_LOGICAL_W/H 1125x2436: stride 4544, 11,069,184 B contiguous. Fail loudly if the allocation fails.
- Uniform downscale with new fOutX=9, fOutW=701, fOutH=1520 in blitScaled (:667-711): replace fPanelW in the x and xmap maths, add fOutX*3 to the destination, clear the panel to black once.
- Byte order follows the open question: if the top band was red (RGB), swap per pixel here and in pe_serial.c fbcon_px (:731).
- Under OLIVE, set sweet_console_muted at the first present (:1365-1372). On a video-mode panel console text would otherwise draw over the GUI. pstore is unaffected (serial_console.c:284-289).
- setPower (:1040-1067): on unblank set fDirty; on blank zero the panel under fPresentLock.
- gpuStart (:743-772) returns false under OLIVE. The 0xc0800000/0xc1000000 buffers fall inside olive xnu RAM.
- GetDotPitch (:1651) returns 458.
- Default sweetfb_hz and sweetfb_vsync_hz to 20.
- Check the #else at :1523: it kicks only if fScaled, and with no kickoff-reg sweet_fbcon_kick returns at pe_serial.c:1154, so it is a no-op.

The ADT emits /sweet-fbcon with NO kickoff-reg (SH-2), so the fbcon kick/DSI paths at pe_serial.c:786-1183 stay inert. boot_args.Video comes from the live RGB0 pipe (SH-1).

SweetHID.cpp:364-375 #else is 1080x1920 (Pixel 1). Add the OLIVE transform: x_c = clamp((x-9)*1125/701), y_c = y*2436/1520. This is needed with touch at M5.


## P0411 0411-olive-wdt.patch: APSS watchdog 0x0b017000 pet or disable [conditional: M2 only if the shim cannot disable it]
files: iokit/Kernel/arm/SweetPlatformExpert.cpp
risk: Low
refs: live.dts:3267-3273, SweetPlatformExpert.cpp:252,283-325

First the shim logs WDT_EN at 0x0b017008 and disables it unless olive_wdt is set (SH-1d). If that holds, this patch is deferred.

Otherwise add a small OLIVE thread in SweetPlatformExpert:
- ml_io_map 0x0b017000;
- pet WDT_RST 0x04 every 5 s;
- the bark time is 0x2af8 ms (live.dts:3267-3273).

On taimen, WDT resets lost pstore (NOTES-taimen.md:706-708).


## P0412 0412-olive-touch-keys-rpm.patch: onclite 0108/0111/0112 retargeted to olive, battery value [M5-REQUIRED]
files: bsd/dev/olive_touch.c (new), bsd/dev/olive_keys.c (new), bsd/dev/olive_rpm.c (new), bsd/conf/files, iokit/Kernel/arm/SweetBattery.cpp, refs/crsvt_xnu-android-port/xnu/patches/0108*,0111*,0112*
risk: Medium
refs: sweet_keys.c:30-49,136, SweetBattery.cpp:43-47, live.dts:1983,2066,10992-11007, 0111:34, 0107:247-280

Port these onclite drivers as olive_* files under ARM64_BOARD_CONFIG_OLIVE, called from olive_board_start (0407):
- onclite_rpm (0112), needed for the touch L10 rail;
- onclite_keys (0111): SPMI_CORE_PA 0x0200f000, PM8953 PON at live.dts:2066;
- onclite_touch (0108): touch IC, I2C bus and GPIOs taken from live.dts; never call the TLMM 0x01010000 sweet_touch.
- Touch attaches its SPI with sweet_spi_attach (0403), routed to the attaching core.

Other items:
- SweetBattery.cpp:43-45: OLIVE capacity 5000 mAh (inferred), replacing Pixel-1's 2770.
- Optional backlight: confirm KTD3137 I2C at 0x37 on Linux first (live.dts:10992-11007). LK leaves the backlight at the splash level, which is enough for M4.
- Optional DSI DCS 0x28/0x29 using 0107 dsiSend at DSI0 0x1a94000, with the packet page in the free splash tail. This needs an IO mapping via ml_io_map.


## P0413 0413-olive-smp.patch: cpus>1 [post-M5]
files: iokit/Kernel/arm/SweetCPU.cpp, osfmk/arm64/sweet_early.c, pexpert/arm/pe_fiq.c
risk: Medium (cache maintenance and IPI ordering)
refs: SweetCPU.cpp:312-363,451-461, arm_init.c:767, sweet_early.c:100-128

With 0404 (c)/(d) and the cpuid.c change from 0401 in place:
- Test cpus=4 first (0x100-0x103), then cpus=8, which brings in 0x000-0x003 (logical 4-7).
- Port onclite's atomic PRZ writer: the sweet_early.c:100-128 ring is not SMP-safe.
- Never use CPU_OFF (SweetCPU.cpp:451-461).
- If the shim watcher core (SH-3) is enabled, the ADT lists 7 CPUs.


## SH-1 Shim hardening in repo/olive/shim (exists; QEMU PASS) [M1-REQUIRED]
files: repo/olive/shim/shim.c, repo/olive/shim/start.S, repo/olive/shim/plog.h, repo/olive/shim/build.sh, olive/probe/probe.c
risk: SPMI/PON addresses come from onclite (also PM8953). PS_HOLD-low may power the phone off instead of resetting it, which loses DDR.
refs: repo/olive/shim/shim.c:127-142,257-283,327-328, repo/olive/README.md, onclite 0105:25-40, live.dts:1690-1695,1983-1991,3267, iomem.txt:110-119

repo/olive/shim/shim.c already finds the SWEETXNU payload through /chosen initrd, copies segments with 8-byte accesses, places the ADT and a 16K-aligned boot_args, and branches at EL1 on 0x100. It is GIC-free: never reuse repo/shim/shim.c's test_gic, which executes ICC_*.

Changes:
(a) shim.c:327-328: a boot MPIDR other than 0x100 calls fail() in the phone build.
(b) PON pre-set before the jump, mandatory before M3:
   - minimal SPMI arb v2: scan APIDs at 0x200f000+0x800+4n for PPID 0x008 and check owner EE 0 at 0x200a000+0x700+4n;
   - write 0x85a warm (x&~0xf|1) and SOFT_RB_SPARE 0x88f = recovery (1<<2);
   - read both back and log APID and owner to the PRZ;
   - refuse to jump if the read-back is wrong, unless olive_unsafe=1;
   - fail() writes the same.
(c) quiesce_spis (:257-283): also log ITARGETSR0 and GICC_HPPIR, and clear PPI pending/active (ICPENDR0 and ICACTIVER0 = 0xffff0000).
(d) WDT: log EN (0x0b017008) and STS (0x0b01700c); if EN=1 and olive_wdt is not given, write EN=0.
(e) Check that [phys_base, +mem_size) lies within one System RAM range (iomem.txt:110,116,117).
(f) Log the load PA, x0 DTB and the initrd range, and assert initrd_end < phys_base. CAF LK's SCRATCH_ADDR may be 0xA0100000 (inferred), so never read the fastboot buffer after writing phys_base.
(g) Fix the start.S:47 vectors comment.
(h) boot_args.Video from the live RGB0 pipe (probe.c:218-245 fb_find): base, stride, w, h, depth 24. Zero it if no pipe points into 0x90000000-0x91400000.
(i) Keep its own PRZ log capped at 0x7f000, so stage 0 cannot reach pmsg.


## SH-2 ADT generator repo/olive/tools/mkadt_olive.py fixes [M2-REQUIRED]
files: repo/olive/tools/mkadt_olive.py, repo/olive/tools/mkadt-olive.patch, repo/tools/mkadt.py
risk: Low. A wrong cpu reg is caught by the 0403 bounds panic.
refs: mkadt_olive.py:14-15,53,73-91,166-182, mkadt.py:23,195-306, machine_routines.c:910-925,1003-1012,1090-1094,1208-1219, KernelConfigTables.cpp:41,47, pe_serial.c:1287-1342

CPUs:
- Order 0x100 (state 'running', cpu-id 0) first, then 0x101-0x103, then 0x000-0x003.
- Explicit cluster-core-id = index (otherwise :1092 defaults it to phys_id>>8), l2-cache-id = Aff1, cluster-type 0.
- Compatible 'arm,cortex-a53', 'ARM,v8' and 'qcom,kryo280' (belt and braces against KernelConfigTables).
- Timebase 19200000.

Root compatible keeps 'xiaomi,sweet'.

GIC node: compatible arm,gic-400, reg u64(0x0a000000, 0x1000, 0x0a002000, 0x1000) relative to SOC_BASE 0x01000000, virtual-timer-intid 20, no redistributor-stride.

/sweet-fbcon: reg <0x90001000 0x322000> (FB_SIZE at :53 rounded up to a page), 720x1520, stride 2160, bpp 24, NO kickoff-reg. With kickoff-reg, pe_serial.c:1309-1342 would write CTL0 START and VIG0 into a running video pipe.

/olive-emmc node for M3. No /sweet-ufs node, ever.

RAM: default 0xa0000000 + 0x5f400000. For the first boot use OLIVE_MEM=0x40000000. Assert base+size <= 0x100000000 and that the size is 2 MB aligned.

Comments: the 'unverified panel' comment (:166-168) is stale. The byte order is still open.


## SH-3 Shim hang-watcher core (taimen pattern) [after M2, before long M3 runs]
files: repo/olive/shim/shim.c, repo/shim/taimen.c, repo/olive/tools/mkadt_olive.py
risk: Low
refs: shim/taimen.c:228-290, mkadt.py:224-226, NOTES-taimen.md:706-708

Port shim/taimen.c:228-290:
- PSCI CPU_ON 0x103 into the shim image at 0x100xxxxx, outside xnu RAM;
- mirror the PRZ;
- after 8 s with no PRZ change, drop PS_HOLD (warm, because of SH-1b) to land in recovery with pstore intact;
- the ADT lists 7 CPUs while it is on;
- a boot-arg disables it for SMP work.


## SH-4 Root image tooling on macOS [M3-REQUIRED]
files: repo/tools/mkiosroot-mac.sh, repo/tools/img2simg.py, repo/tools/taimen/mkrootfix.py
risk: Low
refs: mkrootfix.py:43-53,64-70,98-100, crsvt NOTES.md:182-186

1. Build the image: `sudo repo/tools/mkiosroot-mac.sh OS.dmg SystemOS.dmg AppOS.dmg ios16-root-olive.img 16g` from the 20H392 iPhone X IPSW. The IPSW is not yet located.
2. Sparse it: `python3 -I tools/img2simg.py`. A raw 16 GiB flash fails on LK.
3. Install fastboot: `brew install --cask android-platform-tools` (it is not installed).
4. Port mkrootfix.py to macOS: SEVENZIP=/opt/homebrew/bin/7zz, and winpath() becomes the identity at :43-53/70/100.
5. Before flashing, take Linux SHA-256s of fsc, vbmeta and dtbo for the emmctest comparison.


## V-1 Static validation of the OLIVE kernel before any phone boot [M2 gate]
files: tools/apply-check.sh, build/xnu-OLIVE
risk: Low
refs: SweetCPU.cpp:299, start.s:330-334, pe_fiq.c:17-439

1. Run apply-check.sh with OLIVE (see P00).
2. Link-consistency grep. Every symbol used under SWEET must be defined under OLIVE: sweet_spi_*, sweet_vtimer_intid, sweet_gic_*. The TAIMEN-only symbols (taimen_gic_dump, taimen_fast_map) must not be referenced.
3. Preprocessed diff of the VM headers, TAIMEN vs OLIVE (see 0401).
4. objdump kernel.development.olive:
   - no S3_0_C12_* or ICC_* sysregs outside the dead #if HAS_GIC_V3 code;
   - no casa, ldadd or swp;
   - a post-link erratum 843419 scan: ADRP at VA&0xfff in {0xff8, 0xffc} followed by a load/store (ld64 has no fix);
   - a hand review of commpage_asm.s and machine_routines_asm.s for 835769 patterns.
5. grep the OLIVE preprocessed output for 0x9d400000, 0x9e400000, 0xc800000, 0xc90201c, 0xc0800000, 0x17817000, 0x10ac000 and 0xb0080000. None may remain.
6. `grep -c sweet_hfs` the generated config, to confirm MASTER.arm64.olive was used.


## M1 MILESTONE M1: shim + stub on the phone
files: repo/olive/shim, olive/probe/probe.c
risk: Low (RAM boot only)
refs: repo/olive/README.md, probe.c:277-287

Needs: SH-1 (a, c, d, e, f, i at least; b recommended), SH-2 (CPU and RAM fields), olive/shim/test (stub).

First re-run run-qemu.sh with the new checks:
- PPI pending clear;
- WDT logged;
- a negative case where RAM runs past 4 GiB must FAIL.

Then run `olive/shim/test/build-stub.sh` and `fastboot boot olive/shim/out/olive-stub-boot.img`.

Pass:
- console-ramoops-0 (adb root; cat /sys/fs/pstore/console-ramoops-0; plog-check.py) shows the shim lines, then `STUB RESULT PASS`;
- the PON APID, owner and read-back values are logged;
- WDT_EN is logged.

Also collect: ITARGETSR0 on 0x100, the LK SPI enable/pending state, and the initrd PA under fastboot boot.

Alongside it, run probe v3 (RAM boot):
- log between PSCI and PS_HOLD;
- PS_HOLD-only warm-reset differential;
- read RGB0 UNPACK at 0x1a15034 twice (byte order);
- SGI1 self-test: IAR should read (iface#<<10)|1.


## M2 MILESTONE M2: XNU prints the 'Darwin' banner into ramoops (cpus=1, no root) — minimum first kernel
files: repo/xnu/patches/0400-0406, repo/olive/shim, repo/olive/tools/mkadt_olive.py
risk: 0402 is the riskiest part (MMU-off code).
refs: sleh.c:1964-1968, SweetCPU.cpp:277-364

REQUIRED items: P00, 0400, 0401, 0402, 0403, 0404, 0406 (hub ENXIO only), SH-1, SH-2, V-1.
RECOMMENDED: 0405 (PS_HOLD halt_restart, so panics reset on their own), and 0411 if M1 showed the WDT armed and the shim cannot disable it.
NOT needed: 0407-0410, 0412, 0413.

Payload: mkpayload.py --l2block 0x200000. ADT with OLIVE_MEM=0x40000000.
Boot-args: `cpus=1 debug=0x10000c -noprogress rd=md0 serial=0`. Expect a deliberate no-root panic after the banner.

Expected PRZ sequence:
- shim lines;
- 'SWEET EARLY' stage marks; white bands at y=1400 and 1000+8n;
- fbcon clears the screen;
- the 'Darwin Kernel Version 22.6.0 ... xnu-8796.141.3' banner;
- `OLIVE GIC: GICv2 typer=0x4ea iidr=0x1070 iface=0xNN vtimer=20`;
- `SWEET IRQ 1: intid 20`;
- `SWEET CPU: requested=1 selected=1`;
- the cpu=0 mpidr=0x100 came-alive / SGI1 OK line;
- then the root panic.

Failure map:
- 'expected boot MPIDR' → 0404a
- 'not in ADT' → SH-2 cpu reg
- no IRQ and a hang at the first sleep → vtimer or GICC enable
- an undefined instruction with an ICC encoding → a missed GICv3 sysreg
- 'startup timeout' → SGI1
- no marks at all → 0402 or the pmap.h:114 table count


## M3 MILESTONE M3: eMMC root mounted + launchd
files: repo/xnu/patches/0407-0409, repo/tools/mkiosroot-mac.sh
risk: Userdata formatting by Lineage if the PON pre-set fails.
refs: 0106:1182-1253, crsvt NOTES.md:187-194, NOTES-taimen.md:197

Adds: 0406 (rootfix, mkdir, mem.c guards), 0407, optionally 0408 and 0409, SH-1b (mandatory from here on: no reset may ever land in Lineage), SH-3 (recommended), SH-4. Raise RAM to 0x5f400000 once 1 GiB is stable.

1. Self-test boot: `rd=emmcroot0 emmctest=1`. ramoops should show HC_MODE, caps, host2, CQCFG, the GCC RCGR, EXT_CSD SEC_CNT 122142720, the GPT with userdata at 0x1080000, and the hashes, which must match Linux. Then a fall-through or panic.
2. Flash: `fastboot flash userdata ios16-root-olive.simg`. Keep the shim boot.img as the boot path, and recovery for log pulls.
3. Root boot: `rd=emmcroot0 emmcrw=1 rootrw=1 emmcfirst=0x1080000`, plus the taimen userland boot-args (NOTES-taimen.md:197).

Pass: 'BSD root: emmcroot0', the HFS mount, launchd as PID 1, and taimen_rootfix waiting for `mount -uw /`.


## M4 MILESTONE M4: SpringBoard on screen
files: repo/xnu/patches/0410-olive-display-clcd.patch
risk: The 11 MB contiguous allocation; colour swap.
refs: SweetFramebuffer.cpp:638-711,1355-1372

Adds 0410, plus SH-1h (boot_args.Video from the live pipe). Apply the byte-order swap if M1's UNPACK read or the user's band report says RGB.

Pass:
- AppleCLCD start logs the 1125x2436 shadow and the UNPACK value;
- backboardd swaps;
- the home screen appears downscaled, 701 px wide and centred, with no console text over the GUI.

The backlight stays at LK's level.


## M5 MILESTONE M5: keys + touch
files: repo/xnu/patches/0412-olive-touch-keys-rpm.patch
risk: Medium
refs: SweetHID.cpp:364-375, 0108, 0111, 0112

Adds 0412 and the SweetHID OLIVE transform from 0410.

Pass:
- power and volume keys produce HID events (PM8953 PON over SPMI 0x200f000);
- touch over its SPI interrupt, attached through sweet_spi_attach with the ISENABLER read-back passing;
- taps land correctly after the (x-9)*1125/701 and y*2436/1520 mapping;
- SweetBattery reports the olive capacity.

Next step: 0413 (SMP).


## Open questions
- Byte order of the 24-bpp splash: in probe v2's TOP band (byte0=0xff), did you see blue (B,G,R order, no change needed) or red (R,G,B order, swap needed in blitScaled and fbcon_px)? M1 can settle it by reading RGB0 UNPACK at 0x1a15034.
- Does olive's LK honour PON SOFT_RB_SPARE after a warm PS_HOLD reset, and is PON's SPMI APID owned by EE 0? This gates writing the iOS root (M3). Probe v1 is confounded because it calls PSCI first.
- Does pulling PS_HOLD low on olive warm-reset the phone (DDR and pstore kept) or power it off? If it powers off, the hang watcher and panic reset lose their logs.
- Does LK leave the APSS watchdog at 0x0b017000 armed at handoff? If the shim cannot disable it, 0411 becomes M2-required.
- Under `fastboot boot`, where are the initrd, the kernel PA and the download buffer? CAF SCRATCH_ADDR 0xA0100000 is inferred and would sit inside the xnu window. Also, what is `fastboot getvar max-download-size`?
- Where is the 20H392 iPhone X IPSW (OS.dmg plus the SystemOS/AppOS cryptexes)? It was not found under ~/ios-olive or ~/Downloads.
- Errata policy for Cortex-A53 r0p4: is -mfix-cortex-a53-835769 plus a post-link 843419 scan enough, given that the iOS userland cannot be rebuilt?
- Is `cust` (p61) part of Lineage's retrofit super group? This decides where the RFX apps archive can live.