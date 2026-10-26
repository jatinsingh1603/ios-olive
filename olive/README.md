# olive: Redmi 8 (SDM439) boot shim + ADT

Shim lane of the iOS 16 port (OLIVE = TAIMEN 4K-page xnu + onclite board support).
Nothing here touches the phone; `fastboot boot` is the user's step.

## Files

| path | what |
|---|---|
| `shim/start.S` `shim/shim.ld` | Linux arm64 Image header + entry + vectors (onclite/shim, unchanged apart from comments) |
| `shim/shim.c` | the shim: finds the SWEETXNU payload via `/chosen linux,initrd-start/end`, range-checks it, copies segments to `phys_base + (va - virt_base)`, ADT + 16K boot_args behind the kernel, stops EL1 timers, disables/clears GICv2 SPIs, **branches to xnu on the boot core (MPIDR 0x100) at EL1** |
| `shim/plog.h` | ramoops console writer: zone 0x9ff00000, 0x80000 bytes, sig DBGC; every line ends in ` *XX` checksum (the olive zone flips bits across reset) |
| `shim/util.h` | boot-arg parser, `sum64` (FNV-1a over 8-byte words), `popcount32` |
| `shim/build.sh PAYLOAD` | -> `shim/out/olive-boot.img` (Image.gz + stock DTBs, header copied from `stock/boot.img`); `QEMU=1` -> `shim/out-qemu/Image` |
| `shim/test/stub*.{c,S,ld}` | stand-in kernel: checks what xnu would get, appends PASS/FAIL to the record, resets (recovery reason, then SYSTEM_RESET, then PS_HOLD) |
| `shim/test/mkstubpayload.py` | stub payload: 4K layout (virt_base 0xfffffff007000000, entry +0x4000), zero-fill segment, 16 MB filler; expected values on the command line |
| `shim/test/build-stub.sh` | -> `shim/out/olive-stub-boot.img` (phone) or `QEMU=1` -> `shim/out-qemu/` |
| `shim/test/run-qemu.sh [SECS]` | full shim + stub run in QEMU virt (cortex-a53, GICv2, 4 GiB), dumps the ramoops zone, exit 0 iff `STUB RESULT PASS` |
| `tools/mkadt_olive.py OUT.adt [--dump]` | olive ADT |
| `tools/mkadt-olive.patch` | `git apply` it to get `tools/mkadt.py OUT --olive` (delegates to mkadt_olive.py) |
| `tools/plog-check.py FILE [--text]` | checks the ` *XX` checksums in console-ramoops-0 or a raw zone dump |

## Build / test

```sh
olive/shim/test/run-qemu.sh                 # host test, expect "STUB RESULT PASS", exit 0
EXTRA=olive_qemu_dirty=1 olive/shim/test/run-qemu.sh   # LK-like armed SPIs + ptimer get quiesced
olive/shim/test/build-stub.sh               # phone stub image: olive/shim/out/olive-stub-boot.img
# real kernel (kernel lane): ADT, payload with 2 MB virt_base, then the boot image
python3 -I olive/tools/mkadt_olive.py out/olive.adt
python3 -I tools/mkpayload.py kernel.development.olive out/olive.adt out/payload --l2block 0x200000 --cmdline "debug=0xc cpus=1 keepsyms=1"
olive/shim/build.sh out/payload             # -> olive/shim/out/olive-boot.img
```

On the phone (user's step): `fastboot boot olive/shim/out/olive-stub-boot.img`; the stub resets with the
recovery reason, which olive LK turns into a Lineage boot (probe v1). Then
`adb root; adb pull /sys/fs/pstore/console-ramoops-0` and `olive/tools/plog-check.py console-ramoops-0`.
Expect `SHIM ... branch -> xnu`, `STUB ok ...` lines and `STUB RESULT PASS` (printed twice).

## Shim boot-args (payload command line)

`olive_gicq=0` keep LK's SPIs; `olive_wdt=SECS` (1-31) arm APSS WDT 0x0b017000 (off by default: a bite may
enter download mode, unverified); `olive_hold=1` log everything, do not jump; `olive_qemu_dirty=1` QEMU build only.

## Memory map used (from device/live.dts reserved-memory, device/iomem.txt:110-126, probe v1)

| range | use |
|---|---|
| 0x10080000.. | LK puts the shim Image here (Linux "Kernel code" 0x10080000, inferred same for us; logged as `IMAGE lo`) |
| 0x13400000 / 0x13600000 | DTB / initrd = payload (iomem reserved 13400000-13469fff, 13600000-13921fff = stock ramdisk size; inferred) |
| 0x84a00000-0x8dc00000 | no-map carve-outs (other_ext, modem, adsp_fw, wcnss_fw) - shim refuses overlap |
| 0x8f000000-0x8fffffff | Linux-reserved, owner unknown (LK region on MSM8953-class LK, inferred) - refused |
| 0x90000000+0x1400000 | LK splash; fb 0x90001000 720x1520 stride 2160 RGB888 -> ADT `/sweet-fbcon` (24 bpp, no kickoff: video panel) |
| 0x9ff00000+0x80000 | ramoops console zone (shim, stub, then xnu) |
| **0xa0000000+0x5f400000** | xnu RAM (ADT dram-base/size; `OLIVE_BASE`/`OLIVE_MEM` override, checked against the free list) |
| 0xff400000+0xc00000 | `/onclite-panel` shadow canvas (onclite 0107), never given to the VM |

## What the kernel lane must match (not done here)

- onclite 0102 hard-codes the ramoops zone: `PLOG_PA 0x91400000`, `PLOG_ZONE_SIZE 0x200000` (0102:71-73) and a 32 MB
  block at 0x90000000 (0102:11, 0102:38). OLIVE: 0x9ff00000 / 0x80000, and with the 4K granule the zone sits in the
  2 MB L2 block 0x9fe00000 (the splash needs 0x90000000-0x913fffff = 10 blocks if mapped early).
- Boot CPU = logical 0 = MPIDR 0x100 (ADT cpu0); secondaries are cold OFF (AFFINITY_INFO logged); never CPU_OFF a core
  you want back (onclite probes v3/v4).
- The shim disables every SPI: drivers enable their own SPI (and ITARGETSR) - onclite never relied on LK's.
- No boot video in boot_args (as onclite); fbcon via ADT at 24 bpp (taimen xnu 0079 supports it).
- ADT compatibles: root `D22AP iPhone10,3 xiaomi,sweet xiaomi,olive qcom,sdm439 AppleARM` (0016 matches `xiaomi,sweet`),
  cpus `qcom,kryo280 arm,cortex-a53 ARM,v8` (0195 matches `qcom,kryo280`); onclite 0101 instead matches
  `xiaomi,onclite`/`qcom,kryo250` - port it to `xiaomi,olive` or keep TAIMEN's matches.
- Payload with `mkpayload.py --l2block 0x200000` (the shim refuses a virt_base/phys_base that is not 2 MB aligned).
- Reset through PS_HOLD (onclite 0105), LSE emulation (0103) and `-mcpu=cortex-a53` (A53: no LSE).

## Verified vs inferred

Verified (here): builds with the Arm GNU 14.2 bare-metal toolchain, no ABS64 relocations, no LSE, no FP/SIMD;
QEMU run passes all stub checks (EL1, MMU/caches off, boot core, bss zero fill, ADT + 16 MB filler bit-exact,
boot_args, timers stopped, SPIs quiesced, no secondary started); negative cases (phys_base in the splash, virt_base
not 2 MB aligned) fail cleanly. Boot image header equals stock apart from sizes.
Not verified: anything on the phone; LK honouring IMEM 0x77665500 (fastboot reason in `fail()`); LK `fastboot boot`
size limit on olive (`fastboot getvar max-download-size`); that 0xa0000000-0xffffffff holds no secure carve-out
missing from the DT (Linux used it as System RAM, which is strong but not complete evidence); whether the panel is
lit after `fastboot boot`.
