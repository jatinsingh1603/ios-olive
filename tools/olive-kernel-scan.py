#!/usr/bin/env python3
"""V-1 step 4: scan a linked OLIVE kernel (or any arm64 object) for code a
Cortex-A53 r0p4 (Redmi 8, MIDR 0x410FD034, ARMv8.0) must never execute.

  olive-kernel-scan.py KERNEL [--max N]

KERNEL     kernel.development.olive from the CI artifact (or a .o for a test)

Reports, per class, every hit with its address and enclosing symbol:
  gicv3     ICC_/ICH_ system registers (S3_0_C12_C8-12, S3_4_C12, S3_6_C12):
            olive is GICv2, there is no system-register CPU interface
  lse       ARMv8.1 atomics (cas*, ld<op>*, st<op>*, swp*): ISAR0 0x11120, no LSE
  v8x       other post-v8.0 instructions that UNDEF on the A53: non-hint
            pointer auth (retaa, braa, ldraa, pacga, pacia x0, x1, ...), RCPC
            (ldapr), RDM (sqrdmlah), dot product, PAN/UAO/DIT/SSBS writes,
            dc cvap/cvadp, flag manipulation, MTE, sb
  impdef    S3_x_C15 implementation-defined system registers (Apple HID/ACC
            registers on Apple silicon; CPUACTLR/CPUECTLR/L2CTLR on the A53,
            which may trap from EL1): every one needs a human look
  e843419   erratum 843419 candidates: ADRP at an address with VA & 0xfff in
            {0xff8, 0xffc} whose Xn is the base of a load/store within the next
            three instructions, with another load/store before it. ld64 has
            no fix (GNU ld --fix-cortex-a53-843419 does); a candidate must be
            moved (padding or a code change) or proven harmless by hand
  e835769   erratum 835769 candidates: a 64-bit multiply-accumulate (madd,
            msub, smaddl, smsubl, umaddl, umsubl) directly after a load/store.
            The C code is built with -mfix-cortex-a53-835769; hits come from
            hand-written assembly (commpage_asm.s, machine_routines_asm.s) or
            from libraries built without the flag

Exit status 0 when gicv3, lse, v8x and e843419 are all empty, 1 otherwise.
impdef and e835769 hits are listed for review only: read every one.

Hint-space pointer auth (paciasp, autiasp, pacibsp, autibsp, xpaclri,
pacia1716 ...) executes as NOP on ARMv8.0 and is not reported.
"""
import re, subprocess, sys
from collections import defaultdict

LSE = re.compile(r'^(casp?[al]{0,2}[bh]?|swp[al]{0,2}[bh]?|'
                 r'ld(add|clr|eor|set|smax|smin|umax|umin)[al]{0,2}[bh]?|'
                 r'st(add|clr|eor|set|smax|smin|umax|umin)l?[bh]?)$')
# non-hint pointer authentication and other ARMv8.1+ encodings
V8X = re.compile(r'^(reta[ab]|bra[ab]z?|blra[ab]z?|ereta[ab]|ldra[ab]|pacga|'
                 r'pac(i|d)[ab]z?|pacdz[ab]|aut(i|d)[ab]z?|xpac[id]|'
                 r'ldapr[bh]?|ldapur[bh]?|ldapurs[bhw]|stlur[bh]?|'
                 r'sqrdml[as]h|[su]dot|usdot|sudot|'
                 r'cfinv|axflag|xaflag|rmif|setf8|setf16|'
                 r'irg|gmi|subp|subps|addg|subg|stz?2?g|ldg|stgp|'
                 r'sb|frint(32|64)[xz]|bfcvt|bfdot|bfmmla|[su]mmla)$')
# hint-space forms that are NOPs on v8.0 (paciasp etc.) never match V8X:
# they have no operands and distinct mnemonics (paciasp, autiasp, pacia1716 ...).
HINT_PAC = re.compile(r'^(pac|aut)i[ab](sp|z|1716)$|^xpaclri$')
GICV3_REG = re.compile(r'\b(ICC_\w+|ICH_\w+|ICV_\w+|S3_0_C12_C(8|9|1[0-2])_\d|S3_[46]_C12_C\d+_\d)\b', re.I)
IMPDEF_REG = re.compile(r'\bS3_\d_C15_C\d+_\d\b', re.I)
V8X_SYSREG = re.compile(r'^(msr|mrs)$')
V8X_REGS = re.compile(r'\b(PAN|UAO|DIT|SSBS|TCO|ALLINT)\b')
DC_V8X = re.compile(r'^dc$')
LDST = re.compile(r'^(ld|st)[a-z0-9]*$|^prfu?m$')       # every load/store/prefetch, SIMD ld1/st1 too
MAC64 = re.compile(r'^(madd|msub|smaddl|smsubl|umaddl|umsubl)$')
LINE = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$')
SYM = re.compile(r'^([0-9a-f]+) <(.+)>:$')


def disasm(path):
	p = subprocess.run(['objdump', '-d', '--no-show-raw-insn', path],
	    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors='replace')
	if p.returncode != 0:
		sys.exit(f'olive-kernel-scan: objdump failed: {p.stderr.strip()}')
	sym = '?'
	for line in p.stdout.splitlines():
		m = SYM.match(line)
		if m:
			sym = m.group(2)
			continue
		m = LINE.match(line)
		if m:
			yield int(m.group(1), 16), m.group(2).lower(), m.group(3).split('//')[0].strip(), sym


def base_reg(ops):
	m = re.search(r'\[\s*(x\d+|sp)\b', ops)
	return m.group(1) if m else None


def is_ldst(mn):
	return bool(LDST.match(mn)) or bool(LSE.match(mn))


def main():
	args = sys.argv[1:]
	if not args:
		sys.exit(__doc__)
	path, limit = args[0], 200
	if '--max' in args:
		limit = int(args[args.index('--max') + 1])
	ins = list(disasm(path))
	hits = defaultdict(list)
	for i, (va, mn, ops, sym) in enumerate(ins):
		if LSE.match(mn):
			hits['lse'].append((va, sym, f'{mn} {ops}'))
		elif V8X.match(mn) and not HINT_PAC.match(mn):
			hits['v8x'].append((va, sym, f'{mn} {ops}'))
		elif V8X_SYSREG.match(mn) and V8X_REGS.search(ops):
			hits['v8x'].append((va, sym, f'{mn} {ops}'))
		elif DC_V8X.match(mn) and re.match(r'cvad?p\b', ops):
			hits['v8x'].append((va, sym, f'{mn} {ops}'))
		if mn in ('msr', 'mrs') or mn.startswith('sys'):
			if GICV3_REG.search(ops):
				hits['gicv3'].append((va, sym, f'{mn} {ops}'))
			elif IMPDEF_REG.search(ops):
				hits['impdef'].append((va, sym, f'{mn} {ops}'))
		if mn == 'adrp' and (va & 0xfff) in (0xff8, 0xffc):
			rd = ops.split(',')[0].strip()
			seen_ldst = False
			for va2, mn2, ops2, _ in ins[i + 1:i + 4]:
				if mn2.startswith('b') or mn2 in ('ret', 'cbz', 'cbnz', 'tbz', 'tbnz', 'br', 'blr'):
					break
				if is_ldst(mn2):
					if seen_ldst and base_reg(ops2) == rd:
						hits['e843419'].append((va, sym, f'adrp {ops} ... {mn2} {ops2} @{va2:#x}'))
						break
					seen_ldst = True
		if MAC64.match(mn) and ops.startswith('x') and i > 0 and is_ldst(ins[i - 1][1]):
			hits['e835769'].append((va, sym, f'{ins[i - 1][1]} {ins[i - 1][2]} ; {mn} {ops}'))
	print(f'olive-kernel-scan: {path}: {len(ins)} instructions')
	for cls in ('gicv3', 'lse', 'v8x', 'e843419', 'impdef', 'e835769'):
		h = hits[cls]
		print(f'{cls:8s} {len(h)}')
		for va, sym, text in h[:limit]:
			print(f'    {va:#018x} {sym}: {text}')
		if len(h) > limit:
			print(f'    ... {len(h) - limit} more')
	return 1 if any(hits[c] for c in ('gicv3', 'lse', 'v8x', 'e843419')) else 0


if __name__ == '__main__':
	sys.exit(main())
