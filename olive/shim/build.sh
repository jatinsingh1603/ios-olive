#!/bin/bash
# Build the olive shim and wrap it like the stock kernel: Image.gz + the
# stock DTBs appended, in a boot.img v1 that mirrors the stock header, with
# the xnu payload (tools/mkpayload.py --l2block 0x200000) as the ramdisk:
#   olive/shim/build.sh PAYLOAD            -> olive/shim/out/olive-boot.img
#   QEMU=1 olive/shim/build.sh             -> olive/shim/out-qemu/Image (no boot.img)
# Env: TC (toolchain prefix), STOCK (stock boot.img), STOCK_DTB (its appended
# DTBs, tools/py/unpack_bootv2.py), OUT_NAME, EXTRA_CFLAGS.
# The ramdisk address in the header does not matter: LK puts the initrd
# right after the DTB anyway (onclite NOTES.md; olive iomem.txt shows the
# stock initrd at 0x13600000, DTB at 0x13400000).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$here/../.." && pwd)
OLIVE=${OLIVE:-$(cd "$REPO/.." && pwd)}		# ~/ios-olive
TC=${TC:-$OLIVE/tools/arm-gnu-toolchain-14.2.rel1-darwin-arm64-aarch64-none-elf/bin/aarch64-none-elf-}
STOCK=${STOCK:-$OLIVE/stock/boot.img}
STOCK_DTB=${STOCK_DTB:-$OLIVE/stock/boot_unpacked/dtb}
MKBOOT=${MKBOOT:-$OLIVE/tools/py/mkbootimg-v1.py}
CC=${TC}gcc
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -mcpu=cortex-a53 -Wall -Wextra -Werror ${EXTRA_CFLAGS:-}"
if [ "${QEMU:-0}" = 1 ]; then
	CFLAGS="$CFLAGS -DOLIVE_QEMU"
	O=$here/out-qemu
else
	PAYLOAD=$(realpath "${1:?usage: build.sh PAYLOAD (or QEMU=1 build.sh)}")
	O=$here/out
fi
mkdir -p "$O"
$CC $CFLAGS -c "$here/start.S" -o "$O/start.o"
$CC $CFLAGS -c "$here/shim.c" -o "$O/shim.o"
$CC $CFLAGS -c "$REPO/shim/fdt.c" -o "$O/fdt.o"
# The image runs wherever LK puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(${TC}readelf -r "$O/shim.o" "$O/fdt.o" | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T "$here/shim.ld" -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	"$O/start.o" "$O/shim.o" "$O/fdt.o" -lgcc -o "$O/shim.elf"
# ARMv8.0 only: no LSE atomics etc. may slip in (the A53 has none).
if ${TC}objdump -d "$O/shim.elf" | grep -Eq $'\t(cas|casp|ldadd|ldclr|ldeor|ldset|swp|ldapr)[a-z]*\t'; then
	echo "error: ARMv8.1+ instruction in shim.elf" >&2; exit 1
fi
# No FP/SIMD either: LK leaves CPACR_EL1.FPEN unknown (libgcc's popcount trapped in QEMU).
if ${TC}objdump -d "$O/shim.elf" | grep -Eq $'\t[a-z0-9.]+\t.*(\\bv[0-9]+\\.|\\b[bhsdq][0-9]+\\b)'; then
	echo "error: FP/SIMD register use in $O/shim.elf" >&2; exit 1
fi
${TC}objcopy -O binary "$O/shim.elf" "$O/Image"
size=$(${TC}nm "$O/shim.elf" | awk '$3=="_end"{print $1}')
truncate -s $((16#$size)) "$O/Image"
if [ "${QEMU:-0}" = 1 ]; then
	ls -l "$O/Image"
	exit 0
fi
gzip -9 -n -c "$O/Image" > "$O/Image.gz"
cat "$O/Image.gz" "$STOCK_DTB" > "$O/Image.gz-dtb"
python3 -I "$MKBOOT" --like "$STOCK" --kernel "$O/Image.gz-dtb" --ramdisk "$PAYLOAD" -o "$O/${OUT_NAME:-olive-boot}.img"
# onclite: an 84 MB payload made `fastboot boot` fail ("Booting FAILED usb_read").
# olive's LK download limit is not measured yet: `fastboot getvar max-download-size`.
sz=$(stat -f %z "$O/${OUT_NAME:-olive-boot}.img" 2>/dev/null || stat -c %s "$O/${OUT_NAME:-olive-boot}.img")
[ "$sz" -le $((64 << 20)) ] || echo "warning: boot image is $((sz >> 20)) MB; onclite LK failed at 84 MB" >&2
ls -l "$O/Image" "$O/${OUT_NAME:-olive-boot}.img"
