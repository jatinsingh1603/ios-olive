#!/bin/bash
# Shim test without xnu: stand-in kernel (stub.c) + olive ADT + filler as the
# payload, then the shim boot image around it.
#   olive/shim/test/build-stub.sh          -> olive/shim/out/olive-stub-boot.img (phone)
#   QEMU=1 olive/shim/test/build-stub.sh   -> olive/shim/out-qemu/{Image,stub-payload.bin}
# Env: PAD_MB (filler, default 16), EXTRA (more boot-args for the shim/stub),
# TC, and mkadt_olive.py's OLIVE_* (RAM window etc.).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
SHIM=$(cd "$here/.." && pwd)
REPO=$(cd "$SHIM/../.." && pwd)
OLIVE=${OLIVE:-$(cd "$REPO/.." && pwd)}
TC=${TC:-$OLIVE/tools/arm-gnu-toolchain-14.2.rel1-darwin-arm64-aarch64-none-elf/bin/aarch64-none-elf-}
F="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -mcpu=cortex-a53 -Wall -Wextra -Werror"
if [ "${QEMU:-0}" = 1 ]; then
	F="$F -DOLIVE_QEMU"; O=$SHIM/out-qemu/test
else
	O=$SHIM/out/test
fi
mkdir -p "$O"
${TC}gcc $F -c "$here/stub_start.S" -o "$O/stub_start.o"
${TC}gcc $F -c "$here/stub.c" -o "$O/stub.o"
abs=$(${TC}readelf -r "$O/stub.o" | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: absolute relocations in stub.o" >&2; exit 1; }
${TC}gcc $F -static -T "$here/stub.ld" -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	"$O/stub_start.o" "$O/stub.o" -lgcc -o "$O/stub.elf"
# No FP/SIMD either: LK leaves CPACR_EL1.FPEN unknown (libgcc's popcount trapped in QEMU).
if ${TC}objdump -d "$O/stub.elf" | grep -Eq $'\t[a-z0-9.]+\t.*(\\bv[0-9]+\\.|\\b[bhsdq][0-9]+\\b)'; then
	echo "error: FP/SIMD register use in $O/stub.elf" >&2; exit 1
fi
${TC}objcopy -O binary "$O/stub.elf" "$O/stub.bin"
vms=0x$(${TC}nm "$O/stub.elf" | awk '$3=="_stack_top"{print $1}')
python3 -I "$REPO/olive/tools/mkadt_olive.py" "$O/olive.adt"
python3 -I "$here/mkstubpayload.py" "$O/stub.bin" "$vms" "$O/olive.adt" "$O/stub-payload.bin" \
	--pad "${PAD_MB:-16}" --extra "${EXTRA:-}"
if [ "${QEMU:-0}" = 1 ]; then
	QEMU=1 "$SHIM/build.sh"
	cp "$O/stub-payload.bin" "$SHIM/out-qemu/stub-payload.bin"
else
	OUT_NAME=olive-stub-boot "$SHIM/build.sh" "$O/stub-payload.bin"
fi
