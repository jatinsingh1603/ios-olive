#!/bin/bash
# Preprocess xnu sources with the TAIMEN and the OLIVE MACHINE_FLAGS (makedefs/MakeInc.def)
# and diff the two, without Xcode or a configured build: missing generated/config headers
# (mach_kdp.h, debug.h, MIG output, ...) are stubbed empty, the same stubs for both configs,
# so only the board gates can make the outputs differ. #error lines are tolerated (-E keeps going).
# usage: olive-pp.sh XNU_TREE OUTDIR file.c|file.h ...
#   writes OUTDIR/<name>.TAIMEN.i, OUTDIR/<name>.OLIVE.i and prints the diff per file.
#   For a header, pass a probe .c that includes it and names the macros to expand.
set -uo pipefail
X=$(cd "$1" && pwd); OUT=$(mkdir -p "$2" && cd "$2" && pwd); shift 2
STUB="$OUT/stub"; mkdir -p "$STUB"
COMMON="-target arm64-apple-macos13 -nostdinc -P -DAPPLE -DKERNEL -DKERNEL_PRIVATE -DXNU_KERNEL_PRIVATE
 -DMACH_KERNEL_PRIVATE -DBSD_KERNEL_PRIVATE -DPRIVATE -D__MACHO__=1 -DXNU_PLATFORM_MacOSX -DXNU_TARGET_OS_OSX
 -Darm64 -DARM64 -D__ARM64__ -DLP64 -DPAGE_SIZE_FIXED -DVM_KERNEL_LINK_ADDRESS=0xfffffff007004000
 -UARM_LARGE_MEMORY -D__API__=v4 -mkernel"
F_TAIMEN="-DARM64_BOARD_CONFIG_SWEET -DARM64_BOARD_CONFIG_TAIMEN -mcpu=kryo"
F_OLIVE="-DARM64_BOARD_CONFIG_SWEET -DARM64_BOARD_CONFIG_OLIVE -mcpu=cortex-a53 -mfix-cortex-a53-835769"
INC="-I $STUB -I osfmk/libsa -I osfmk -I bsd -I libkern -I iokit -I pexpert -I EXTERNAL_HEADERS -I libsa -I security -I san"
pp() {	# config src out: stub every missing header until clang -E gets through
	local i err h
	for i in $(seq 1 300); do
		err=$(cd "$X" && clang -E $COMMON $(eval echo \$F_$1) $INC "$2" 2>&1 >"$3" | grep -m1 "fatal error: '.*' file not found")
		[ -z "$err" ] && return 0
		h=$(echo "$err" | sed -E "s/.*fatal error: '([^']*)'.*/\1/")
		mkdir -p "$STUB/$(dirname "$h")"; : >"$STUB/$h"
	done
	echo "olive-pp: gave up on $2" >&2; return 1
}
rc=0
for f in "$@"; do
	case "$f" in /*) src=$f ;; *) src=$X/$f ;; esac
	n=$(basename "$f"); n=${n%.*}
	pp TAIMEN "$src" "$OUT/$n.TAIMEN.i" && pp OLIVE "$src" "$OUT/$n.OLIVE.i" || { rc=1; continue; }
	d=$(diff "$OUT/$n.TAIMEN.i" "$OUT/$n.OLIVE.i" | grep '^[<>]' | grep -v '^[<>] *$')
	if [ -z "$d" ]; then echo "SAME   $f"; else echo "DIFFER $f"; echo "$d" | sed 's/^/    /'; rc=2; fi
done
exit $rc
