/*
 * olive (Redmi 8, SDM439) boot shim: act as iBoot for the OLIVE xnu
 * (TAIMEN-derived 4K-page kernel, onclite board support).
 *
 * Derived from onclite/shim/shim.c (crsvt xnu-android-port) and
 * shim/loader.c (taimen). Differences from onclite, all deliberate:
 *   - No Gold-core hand-off. LK already runs on MPIDR 0x100 (probe v1:
 *     BOOTCPU mpidr=00000100) and every A53 has the 4K granule xnu needs,
 *     so xnu starts on this core by a plain branch, like the taimen
 *     loader (shim/loader.c:151). No PSCI CPU_ON/CPU_OFF here at all:
 *     onclite probes v3/v4 showed a core that did CPU_OFF never runs again
 *     after a second CPU_ON, so the shim leaves the 7 secondaries exactly
 *     as LK did (OFF, never started); xnu CPU_ONs them from cold.
 *     AFFINITY_INFO (read-only) logs their state.
 *   - No Gold clock switch (SDM632-only, clock-cpu-sdm632.c).
 *   - Ramoops console at 0x9ff00000, 0x80000 bytes (plog.h).
 *   - The core xnu runs on is LK's own, so whatever LK left armed is now
 *     xnu's problem: the shim stops both EL1 timers and (default on,
 *     olive_gicq=0 to skip) disables + clears every SPI in the GICv2
 *     distributor. On onclite the cpu0-targeted SPIs could never reach the
 *     Gold core; here they would. SGIs/PPIs are banked and xnu's GICv2 init
 *     (onclite 0101:299-300) already resets them.
 *   - 4K-page kernel: boot_args stay 16K-aligned as in taimen loader.c:120
 *     (harmless over-alignment, docs/taimen-4k-catalogue.md (D)); the
 *     payload must come from mkpayload.py --l2block 0x200000 (virt_base
 *     2 MB aligned). The shim checks that.
 *   - Range checks: segments, ADT and boot_args must lie in xnu's RAM, and
 *     xnu's RAM must miss every olive carve-out, the shim, the DTB and the
 *     payload itself.
 * Optional boot-args (payload command line, read here, also seen by xnu):
 *   olive_gicq=0     leave the distributor's SPIs as LK left them
 *   olive_wdt=SECS   arm the APSS watchdog (qcom,wdt@b017000) before the
 *                    jump; 1..31 s. Off by default: on Qualcomm a bite can
 *                    end in download mode instead of a reboot (unverified
 *                    on olive).
 *   olive_hold=1     stop in WFE after logging, never jump (dry run)
 *   olive_qemu_dirty=1  (QEMU build only) arm SPIs + ptimer first, to test the quiesce
 *
 * Build with -DOLIVE_QEMU for QEMU virt (gic-version=2, -m 4G): PSCI by
 * HVC, GIC at 0x08000000/0x08010000, no IMEM / PS_HOLD / WDT writes.
 */
#include <stdint.h>
#include "../../shim/fdt.h"
#include "plog.h"
#include "util.h"

#ifdef OLIVE_QEMU
#define GICD_BASE		0x08000000UL
#define GICC_BASE		0x08010000UL
#define BOOT_MPIDR		0x000UL
#define PSCI_INSN		"hvc #0"
#else
#define GICD_BASE		0x0b000000UL	/* qcom,msm-qgic2 (live.dts, probe v1) */
#define GICC_BASE		0x0b002000UL
#define BOOT_MPIDR		0x100UL		/* probe v1 BOOTCPU */
#define PSCI_INSN		"smc #0"
#endif
#define GICD_CTLR		0x000
#define GICD_TYPER		0x004
#define GICD_ISENABLER		0x100
#define GICD_ICENABLER		0x180
#define GICD_ICPENDR		0x280
#define GICD_ICACTIVER		0x380
#define GICC_CTLR		0x000

#define IMEM_RESTART_REASON	0x0860065cUL	/* qcom,msm-imem restart_reason@65c */
#define RESTART_BOOTLOADER	0x77665500U
#define PSHOLD			0x004ab000UL
#define APSS_WDT		0x0b017000UL	/* qcom,wdt@b017000 (live.dts:3267), 32768 Hz */
#define WDT_RST			0x04
#define WDT_EN			0x08
#define WDT_BARK		0x10
#define WDT_BITE		0x14

#define PSCI_VERSION		0x84000000UL
#define PSCI_AFFINITY_INFO	0xc4000004UL
#define PSCI_SYSTEM_RESET	0x84000009UL

#define BOOT_LINE_LENGTH	608
#define BA_ALIGN		0x4000UL	/* taimen loader.c:120 */
#define L2_BLOCK_4K		0x200000UL	/* 4K granule L2 block */

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define dsb() __asm__ volatile("dsb sy" ::: "memory")
#define isb() __asm__ volatile("isb" ::: "memory")

static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }

extern char _head[], _end[];

#define T "SHIM"
static void line(const char *a) { plog_line(T, a, 0); }
static void hex(const char *label, uint64_t v) { plog_hex(T, label, v); }

/* ---- PSCI ---- */

static uint64_t psci(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile(PSCI_INSN
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static void delay_ms(unsigned ms)
{
	isb();
	uint64_t end = rsr("cntpct_el0") + rsr("cntfrq_el0") / 1000 * ms;
	while (rsr("cntpct_el0") < end)
		;
}

/*
 * Reset to fastboot (IMEM reason as onclite; olive LK honouring 0x77665500
 * is NOT verified: probe v1 only tried the recovery reason, which LK
 * turned into a normal Lineage boot). PSCI SYSTEM_RESET, then PS_HOLD low,
 * which onclite needed (SYSTEM_RESET alone did not reset after CPU_OFF).
 */
static void __attribute__((noreturn)) fail(const char *why)
{
	plog_line(T, "FAIL", why);
	line("reset to fastboot");
	delay_ms(500);
#ifndef OLIVE_QEMU
	wr32(IMEM_RESTART_REASON, RESTART_BOOTLOADER);
	dsb();
#endif
	psci(PSCI_SYSTEM_RESET, 0, 0, 0);
#ifndef OLIVE_QEMU
	wr32(PSHOLD, 0);
#endif
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- exceptions: anything here is a shim bug ---- */

static const char *stage;	/* set at runtime: no absolute pointers in data */

void exc_handler(uint64_t slot, uint64_t *regs)
{
	hex("EXCEPTION slot", slot);
	plog_line(T, "stage", stage);
	hex("ESR", rsr("esr_el1"));
	hex("ELR", rsr("elr_el1"));
	hex("FAR", rsr("far_el1"));
	hex("X0", regs[0]);
	fail("exception");
}

/* ---- payload (tools/mkpayload.py) and boot_args (pexpert/pexpert/arm64/boot.h) ---- */

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* MMU off: everything is Device memory, so copy in aligned 8-byte words. */
static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

static int magic_ok(const char *m)
{
	static const char want[] = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

static int overlaps(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
	return a < b + blen && b < a + alen;
}

static int inside(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
	return a >= b && alen <= blen && a - b <= blen - alen;
}

/*
 * Never part of xnu's RAM (live.dts reserved-memory, device/iomem.txt):
 * the no-map carve-outs other_ext 0x84a00000+0x1d00000, modem 0x86800000+
 * 0x5500000, adsp_fw 0x8bd00000+0x1800000, wcnss_fw 0x8d500000+0x700000
 * (one run 0x84a00000-0x8dc00000, minus the 1 MB System RAM hole at
 * 0x86700000 which we do not use either), the Linux-reserved 0x8f000000-
 * 0x8fffffff next to the splash (owner unknown: LK image region on
 * MSM8953-class LK, inferred), splash 0x90000000+0x1400000 (the panel
 * scans it out) and ramoops 0x9ff00000+0x100000. Integers only: no
 * relocations.
 */
static const uint64_t olive_resv[][2] = {
	{ 0x84a00000UL, 0x09200000UL },
	{ 0x8f000000UL, 0x01000000UL },
	{ 0x90000000UL, 0x01400000UL },
	{ 0x9ff00000UL, 0x00100000UL },
};

static void quiesce_timers(void)
{
	hex("CNTP_CTL", rsr("cntp_ctl_el0"));
	hex("CNTV_CTL", rsr("cntv_ctl_el0"));
	wsr("cntp_ctl_el0", 0);
	wsr("cntv_ctl_el0", 0);
	isb();
}

/* Disable and clear every SPI (INTID >= 32). Group 0 bits are RAZ/WI from NS. */
static void quiesce_spis(void)
{
	uint32_t typer = rd32(GICD_BASE + GICD_TYPER);
	unsigned nregs = (typer & 0x1f) + 1;	/* ITLinesNumber: 32 * (N + 1) INTIDs */
	unsigned enabled = 0;
	hex("GICD_CTLR", rd32(GICD_BASE + GICD_CTLR));
	hex("GICD_TYPER", typer);
	hex("GICC_CTLR", rd32(GICC_BASE + GICC_CTLR));
	hex("ISENABLER0", rd32(GICD_BASE + GICD_ISENABLER));
	for (unsigned r = 1; r < nregs; r++) {
		uint32_t en = rd32(GICD_BASE + GICD_ISENABLER + 4 * r);
		if (en) {
			hex("SPI enabled reg", r);
			hex("SPI enabled bits", en);
		}
		enabled += popcount32(en);
		wr32(GICD_BASE + GICD_ICENABLER + 4 * r, 0xffffffffU);
		wr32(GICD_BASE + GICD_ICPENDR + 4 * r, 0xffffffffU);
		wr32(GICD_BASE + GICD_ICACTIVER + 4 * r, 0xffffffffU);
	}
	dsb();
	uint32_t left = 0;
	for (unsigned r = 1; r < nregs; r++)
		left += popcount32(rd32(GICD_BASE + GICD_ISENABLER + 4 * r));
	hex("SPIs enabled by LK", enabled);
	hex("SPIs enabled after", left);
}

#ifndef OLIVE_QEMU
static void arm_wdt(uint64_t secs)
{
	if (secs < 1 || secs > 31) {
		hex("WDT secs out of range, not armed", secs);
		return;
	}
	uint32_t ticks = (uint32_t)secs * 32768u;	/* 20-bit counter, 32 s max */
	wr32(APSS_WDT + WDT_BARK, ticks);
	wr32(APSS_WDT + WDT_BITE, ticks);
	wr32(APSS_WDT + WDT_RST, 1);
	wr32(APSS_WDT + WDT_EN, 1);
	dsb();
	hex("WDT armed secs", secs);
	hex("WDT bite reg", rd32(APSS_WDT + WDT_BITE));
}
#endif

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

void shim_main(uint64_t dtb)
{
	uint64_t rd_start, rd_end, v;

	stage = "start";
	plog_init();
#ifdef OLIVE_QEMU
	line("olive shim v1 (QEMU build)");
#else
	line("olive shim v1");
#endif
	uint64_t self = (uint64_t)(uintptr_t)_head, self_end = (uint64_t)(uintptr_t)_end;
	hex("IMAGE lo", self);
	hex("IMAGE hi", self_end);
	hex("DTB", dtb);
	uint64_t mpidr = rsr("mpidr_el1");
	hex("MPIDR", mpidr);
	hex("EL", rsr("CurrentEL") >> 2);
	hex("SCTLR", rsr("sctlr_el1"));
	hex("CNTFRQ", rsr("cntfrq_el0"));
	if ((rsr("CurrentEL") >> 2) != 1)
		fail("not at EL1");
	if ((mpidr & 0xffffff) != BOOT_MPIDR)
		line("WARN boot core is not the expected MPIDR: ADT cpu0 will not match");

	/* Read-only: 0 ON, 1 OFF, 2 ON_PENDING; xnu CPU_ONs the OFF ones later. */
	stage = "psci";
	hex("PSCI version", psci(PSCI_VERSION, 0, 0, 0));
	for (unsigned c = 0; c < 8; c++) {
		uint64_t m = (c < 4) ? (0x100 | c) : (c - 4);
		char lab[] = "AFFINITY cpu_ (0x___)";
		lab[12] = '0' + c;
		lab[17] = '0' + (m >> 8);
		lab[18] = '0';
		lab[19] = '0' + (m & 0xf);
		hex(lab, psci(PSCI_AFFINITY_INFO, m, 0, 0));
	}

	stage = "initrd";
	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end))
		fail("no initrd in /chosen");
	hex("INITRD lo", rd_start);
	hex("INITRD hi", rd_end);

	const struct payload *p = (const struct payload *)rd_start;
	if (rd_end - rd_start < sizeof(struct payload) || !magic_ok(p->magic))
		fail("no SWEETXNU payload");
	if (p->version != 1 || (p->adt_off & 7) || (rd_start & 7))
		fail("bad payload header");
	if (p->nsegs == 0 || p->nsegs > 64 ||
	    sizeof(struct payload) + 32 * (uint64_t)p->nsegs > rd_end - rd_start ||
	    p->adt_off + p->adt_size > rd_end - rd_start)
		fail("payload header does not fit the initrd");

	uint64_t pa_of_va_base = p->phys_base - p->virt_base;	/* pa = va + this */
	hex("PHYS base", p->phys_base);
	hex("PHYS size", p->mem_size);
	hex("VIRT base", p->virt_base);
	hex("ENTRY va", p->entry_va);
	if ((p->virt_base & (L2_BLOCK_4K - 1)) || (p->phys_base & (L2_BLOCK_4K - 1)))
		fail("virt_base/phys_base not 2 MB aligned (mkpayload.py --l2block 0x200000)");

	stage = "ranges";
	const uint64_t ram = p->phys_base, ram_len = p->mem_size;
	if (ram_len == 0 || ram + ram_len < ram)
		fail("bad RAM window");
	for (unsigned i = 0; i < sizeof(olive_resv) / sizeof(olive_resv[0]); i++)
		if (overlaps(ram, ram_len, olive_resv[i][0], olive_resv[i][1])) {
			hex("carve-out", olive_resv[i][0]);
			fail("xnu RAM overlaps an olive carve-out");
		}
	if (overlaps(ram, ram_len, rd_start, rd_end - rd_start))
		fail("payload (initrd) overlaps xnu RAM");
	if (overlaps(ram, ram_len, self, self_end - self))
		fail("shim image overlaps xnu RAM");
	/* FDT header: magic 0xd00dfeed, totalsize, both big endian. */
	uint64_t dtb_len = 0x100000;
	if ((dtb & 3) == 0 && __builtin_bswap32(rd32(dtb)) == 0xd00dfeedU)
		dtb_len = __builtin_bswap32(rd32(dtb + 4));
	hex("DTB size", dtb_len);
	if (overlaps(ram, ram_len, dtb, dtb_len))
		fail("DTB overlaps xnu RAM");
	if (!inside(p->kernel_end + pa_of_va_base, 0, ram, ram_len))
		fail("kernel_end outside xnu RAM");

	uint64_t entry_pa = p->entry_va + pa_of_va_base;
	int entry_seen = 0;
	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uint64_t dst = s->vmaddr + pa_of_va_base;
		if (s->file_size > s->vmsize || (dst & 7) || (s->file_off & 7) ||
		    s->file_off + s->file_size > rd_end - rd_start ||
		    !inside(dst, s->vmsize, ram, ram_len) ||
		    s->vmaddr + s->vmsize > p->kernel_end) {
			hex("bad segment", i);
			fail("segment outside payload or xnu RAM");
		}
		if (entry_pa >= dst && entry_pa < dst + s->file_size)
			entry_seen = 1;
	}
	if (!entry_seen)
		fail("entry point in no segment");

	stage = "segments";
	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		copy64(dst, rd_start + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}
	hex("SEGS n", p->nsegs);

	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	stage = "boot_args";
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	uintptr_t ba_pa = (adt_pa + p->adt_size + BA_ALIGN - 1) & ~(BA_ALIGN - 1);
	uintptr_t top = ba_pa + BA_ALIGN;
	if (!inside(adt_pa, top - adt_pa, ram, ram_len))
		fail("ADT/boot_args do not fit in xnu RAM");
	copy64(adt_pa, rd_start + p->adt_off, p->adt_size);

	zero64(ba_pa, BA_ALIGN);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = top;
	/*
	 * No boot video (as onclite): the LK splash is RGB888 and xnu's video
	 * console wants 32 bpp. The kernel's console finds the splash through
	 * the ADT /sweet-fbcon node instead (bits-per-pixel 24, xnu 0079).
	 */
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
	ba->memSizeActual = p->mem_size;
	hex("ADT pa", adt_pa);
	hex("ADT len", p->adt_size);
	hex("ADT sum64", sum64(adt_pa, p->adt_size));
	hex("BOOTARG pa", ba_pa);
	hex("TOP", top);

	stage = "quiesce";
#ifdef OLIVE_QEMU
	/* Test only: stand in for LK's armed SPIs and ptimer so the quiesce has work. */
	if (cmdline_val(p->cmdline, "olive_qemu_dirty", &v) && v) {
		wr32(GICD_BASE + GICD_ISENABLER + 4, 0x00f0f0f0U);
		wsr("cntp_ctl_el0", 1);
		isb();
	}
#endif
	quiesce_timers();
	if (cmdline_val(p->cmdline, "olive_gicq", &v) && v == 0)
		line("GIC SPIs left as LK left them (olive_gicq=0)");
	else
		quiesce_spis();
#ifndef OLIVE_QEMU
	if (cmdline_val(p->cmdline, "olive_wdt", &v))
		arm_wdt(v);
#endif

	stage = "handoff";
	hex("JUMP pa", entry_pa);
	if (cmdline_val(p->cmdline, "olive_hold", &v) && v) {
		line("olive_hold: not jumping");
		for (;;)
			__asm__ volatile("wfe");
	}
	/* Last line: xnu (or the stub) appends to this record from here on. */
	line("branch -> xnu on this core, EL1, MMU off, x0 = boot_args");
	/* Everything was written with the MMU and caches off; drop any stale I-cache lines too. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	fail("xnu returned");
}
