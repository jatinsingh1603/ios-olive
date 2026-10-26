/*
 * olive shim test stub: checks, from where xnu's __start would run, what
 * the shim handed over, appends the verdict to the shim's ramoops record,
 * then resets (phone) or parks (QEMU, the host reads RAM).
 *
 * The expected values come in through the command line, written by
 * mkstubpayload.py: pb/ms/vb (phys_base, mem_size, virt_base), adtlen +
 * adtsum (the ADT exactly as packed), padva/padlen/padsum (an 8+ MB filler
 * segment: the copy is bit-exact), zva/zlen (a segment with file_size 8 and
 * vmsize zlen: the shim's zero fill). The stub's own .bss is also only
 * zeroed by the shim (vmsize > file_size).
 *
 * Reset (phone): IMEM restart reason "recovery" + PSCI SYSTEM_RESET +
 * PS_HOLD low, the sequence probe v1 used: LK then boots Lineage, where the
 * record is /sys/fs/pstore/console-ramoops-0.
 */
#include <stdint.h>
#include "../plog.h"
#include "../util.h"

#ifdef OLIVE_QEMU
#define GICD_BASE		0x08000000UL
#define GICC_BASE		0x08010000UL
#define BOOT_MPIDR		0x000UL
#define PSCI_INSN		"hvc #0"
#else
#define GICD_BASE		0x0b000000UL
#define GICC_BASE		0x0b002000UL
#define BOOT_MPIDR		0x100UL
#define PSCI_INSN		"smc #0"
#endif
#define IMEM_RESTART_REASON	0x0860065cUL
#define RESTART_RECOVERY	0x77665502U
#define PSHOLD			0x004ab000UL
#define PSCI_AFFINITY_INFO	0xc4000004UL
#define PSCI_SYSTEM_RESET	0x84000009UL

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	uint64_t video[6];
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[OLIVE_LINE];
	uint64_t bootFlags, memSizeActual;
};
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");

static uint64_t bss_probe[64];	/* zeroed by the shim only */
static unsigned nfail;

#define T "STUB"
static void hex(const char *l, uint64_t v) { plog_hex(T, l, v); }

static void check(int ok, const char *what)
{
	plog_line(T, ok ? "ok  " : "FAIL", what);
	if (!ok)
		nfail++;
}

static uint64_t psci(uint64_t fn, uint64_t a1)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = 0;
	register uint64_t x3 __asm__("x3") = 0;
	__asm__ volatile(PSCI_INSN : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
			 "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			 "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t want(const char *cl, const char *key)
{
	uint64_t v = 0;
	if (!cmdline_val(cl, key, &v)) {
		plog_line(T, "FAIL missing on command line:", key);
		nfail++;
	}
	return v;
}

static void __attribute__((noreturn)) finish(void)
{
	/* Twice: the olive ramoops zone flips bits across the reset (NOTES.md). */
	for (int i = 0; i < 2; i++) {
		hex("RESULT failures", nfail);
		plog_line(T, nfail ? "RESULT FAIL" : "RESULT PASS", 0);
	}
#ifdef OLIVE_QEMU
	plog_line(T, "parked (QEMU)", 0);
	for (;;)
		__asm__ volatile("wfe");
#else
	plog_line(T, "reset (recovery reason -> LK boots Lineage)", 0);
	uint64_t end = rsr("cntpct_el0") + rsr("cntfrq_el0") / 2;
	while (rsr("cntpct_el0") < end)
		;
	*(volatile uint32_t *)IMEM_RESTART_REASON = RESTART_RECOVERY;
	__asm__ volatile("dsb sy" ::: "memory");
	psci(PSCI_SYSTEM_RESET, 0);
	*(volatile uint32_t *)PSHOLD = 0;
	for (;;)
		__asm__ volatile("wfe");
#endif
}

void stub_exc(uint64_t slot)
{
	hex("EXCEPTION slot", slot);
	hex("ESR", rsr("esr_el1"));
	hex("ELR", rsr("elr_el1"));
	hex("FAR", rsr("far_el1"));
	nfail++;
	finish();
}

void stub_main(uint64_t x0)
{
	const volatile struct boot_args *ba = (const volatile struct boot_args *)x0;
	const char *cl = (const char *)ba->CommandLine;
	extern char _stub_entry[];

	plog_line(T, "entered (stand-in for xnu __start)", 0);
	uint64_t mpidr = rsr("mpidr_el1"), sctlr = rsr("sctlr_el1");
	hex("x0 (boot_args pa)", x0);
	hex("PC", (uint64_t)(uintptr_t)_stub_entry);
	hex("MPIDR", mpidr);
	hex("SCTLR", sctlr);
	check((mpidr & 0xffffff) == BOOT_MPIDR, "on the boot core (no hand-off to another CPU)");
	check((rsr("CurrentEL") >> 2) == 1, "EL1");
	check((sctlr & ((1 << 0) | (1 << 2) | (1 << 12))) == 0, "MMU, D-cache, I-cache off");
	check((rsr("daif") & 0x3c0) == 0x3c0, "DAIF masked");
	check((rsr("cntv_ctl_el0") & 1) == 0 && (rsr("cntp_ctl_el0") & 1) == 0, "EL1 timers stopped");

	int bss_zero = 1;
	for (unsigned i = 0; i < sizeof(bss_probe) / sizeof(bss_probe[0]); i++)
		bss_zero &= ((volatile uint64_t *)bss_probe)[i] == 0;
	check(bss_zero, "stub .bss zeroed by the shim");

	hex("Revision", ba->Revision);
	hex("physBase", ba->physBase);
	hex("memSize", ba->memSize);
	hex("virtBase", ba->virtBase);
	hex("top", ba->topOfKernelData);
	check(ba->Revision == 2 && ba->Version == 2, "boot_args Revision/Version 2");
	check(ba->physBase == want(cl, "pb"), "physBase");
	check(ba->memSize == want(cl, "ms") && ba->memSizeActual == ba->memSize, "memSize");
	check(ba->virtBase == want(cl, "vb"), "virtBase");
	check(x0 >= ba->physBase && x0 + 0x4000 == ba->topOfKernelData && (x0 & 0x3fff) == 0,
	      "boot_args 16K aligned, topOfKernelData right after");
	check(ba->video[0] == 0, "no boot video");

	uint64_t adt_pa = ba->deviceTreeP - ba->virtBase + ba->physBase;
	hex("ADT pa", adt_pa);
	hex("ADT len", ba->deviceTreeLength);
	uint64_t s = sum64(adt_pa, ba->deviceTreeLength);
	hex("ADT sum64", s);
	check(ba->deviceTreeLength == want(cl, "adtlen") && s == want(cl, "adtsum"), "ADT bit-exact");
	check(adt_pa + ba->deviceTreeLength <= x0, "ADT below boot_args");

	uint64_t pad_pa = want(cl, "padva") - ba->virtBase + ba->physBase;
	uint64_t pad_len = want(cl, "padlen");
	s = sum64(pad_pa, pad_len);
	hex("PAD pa", pad_pa);
	hex("PAD sum64", s);
	check(s == want(cl, "padsum"), "filler segment bit-exact");

	uint64_t z_pa = want(cl, "zva") - ba->virtBase + ba->physBase, z_len = want(cl, "zlen");
	volatile const uint64_t *z = (volatile const uint64_t *)z_pa;
	int zok = z[0] == 0x5a45524f46494c4cULL;	/* "LLIFOREZ": the 8 file bytes */
	for (uint64_t i = 1; i < z_len / 8; i++)
		zok &= z[i] == 0;
	check(zok, "zero-fill segment (8 file bytes, rest zero)");

	/* Secondaries untouched: everything but the boot core still OFF (1). */
	unsigned on = 0;
	for (unsigned c = 0; c < 8; c++) {
		uint64_t m = (c < 4) ? (0x100 | c) : (c - 4);
		uint64_t st = psci(PSCI_AFFINITY_INFO, m);
		if (m == BOOT_MPIDR)
			continue;
		if (st == 0 || st == 2)
			on++;
	}
	check(on == 0, "no secondary core started");

	uint32_t typer = *(volatile uint32_t *)(GICD_BASE + 4);
	unsigned spis = 0;
	for (unsigned r = 1; r < (typer & 0x1f) + 1; r++)
		spis += popcount32(*(volatile uint32_t *)(GICD_BASE + 0x100 + 4 * r));
	hex("SPIs enabled", spis);
	uint64_t q = 1;
	cmdline_val(cl, "olive_gicq", &q);
	check(q == 0 || spis == 0, "GIC SPIs disabled unless olive_gicq=0");
	hex("GICC_CTLR", *(volatile uint32_t *)(GICC_BASE + 0));

	plog_line(T, "cmdline", 0);
	char c[100];
	int i;
	for (i = 0; i < 99 && cl[i]; i++)
		c[i] = cl[i];
	c[i] = 0;
	plog_raw(c);
	plog_raw("\n");
	finish();
}
