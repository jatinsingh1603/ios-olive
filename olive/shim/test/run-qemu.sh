#!/bin/bash
# Run the shim + stub in QEMU virt (cortex-a53, GICv2, 4 GiB so 0x9ff00000
# and xnu's 0xa0000000 window are RAM), entered like LK enters it: EL1,
# MMU off, x0 = DTB with /chosen linux,initrd-start/end. After SECS the
# ramoops console zone is dumped and checked. Exit 0 iff the stub said PASS.
#   olive/shim/test/run-qemu.sh [SECS]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
SHIM=$(cd "$here/.." && pwd)
REPO=$(cd "$SHIM/../.." && pwd)
QEMU=1 "$here/build-stub.sh" >/dev/null
Q=$SHIM/out-qemu
rm -f "$Q/ramoops.bin" "$Q/mon.sock"
qemu-system-aarch64 -M virt,gic-version=2,secure=off,virtualization=off -cpu cortex-a53 -smp 1 -m 4G \
	-kernel "$Q/Image" -initrd "$Q/stub-payload.bin" -display none -serial none \
	-monitor unix:"$Q/mon.sock",server,nowait -d guest_errors,unimp -D "$Q/qemu.log" &
qpid=$!
trap 'kill $qpid 2>/dev/null || true' EXIT
sleep "${1:-4}"
python3 -I - "$Q/mon.sock" "$Q/ramoops.bin" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1])
def cmd(c):
    s.sendall(c.encode() + b'\n'); time.sleep(0.5); return s.recv(65536)
cmd('')
cmd(f'pmemsave 0x9ff00000 0x80000 "{sys.argv[2]}"')
cmd('info registers')
s.sendall(b'quit\n')
PY
wait $qpid 2>/dev/null || true
trap - EXIT
python3 -I "$REPO/olive/tools/plog-check.py" "$Q/ramoops.bin"
grep -q "STUB RESULT PASS" <(python3 -I "$REPO/olive/tools/plog-check.py" --text "$Q/ramoops.bin")
