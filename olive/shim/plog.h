/*
 * olive ramoops console writer, shared by the shim and the test stub.
 *
 * Linux 4.19 fs/pstore/ram.c on olive: DT pstore_reserve_mem_region
 * reg <0x9ff00000 0x100000>, record-size 0 (no dump zones), console-size
 * 0x80000, pmsg-size 0x8000. ramoops_probe() places the dump zones first
 * (none here), so the console zone is the first 0x80000 bytes, as probe v1
 * verified (device/probe-v1.txt was read back from console-ramoops-0).
 *
 * struct persistent_ram_buffer { u32 sig; atomic_t start; atomic_t size;
 * u8 data[]; } (fs/pstore/ram_core.c); no ECC (no ecc-size in the DT).
 * We never wrap: start == size == bytes written.
 *
 * The olive DDR flips bits in this zone across the warm reset (probe v1:
 * about one bad byte in 20, NOTES.md). Every line therefore ends in
 * " *XX", the low byte of the sum of the line's bytes before " *";
 * olive/tools/plog-check.py marks lines whose checksum does not match.
 * Header only: both images are linked with no runtime relocation, and
 * static inline keeps every pointer PC-relative.
 */
#pragma once
#include <stdint.h>

/* Same address in the QEMU test (virt -m 4G: RAM 0x40000000-0x13fffffff). */
#define RAMOOPS_CONSOLE		0x9ff00000UL
#define RAMOOPS_CONSOLE_SIZE	0x80000UL
#define PRZ_SIG			0x43474244U	/* "DBGC" */
#define PRZ_HDR			12

static inline void plog_wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline uint32_t plog_rd32(uintptr_t a) { return *(volatile uint32_t *)a; }

/* Fresh record (shim): the previous boot's record is gone after this. */
static inline void plog_init(void)
{
	plog_wr32(RAMOOPS_CONSOLE, PRZ_SIG);
	plog_wr32(RAMOOPS_CONSOLE + 4, 0);
	plog_wr32(RAMOOPS_CONSOLE + 8, 0);
	__asm__ volatile("dsb sy" ::: "memory");
}

/* Append (stub, and xnu later): continue at the header's size. */
static inline uint32_t plog_pos(void)
{
	uint32_t n = plog_rd32(RAMOOPS_CONSOLE + 8);
	if (plog_rd32(RAMOOPS_CONSOLE) != PRZ_SIG || n > RAMOOPS_CONSOLE_SIZE - PRZ_HDR) {
		plog_init();
		n = 0;
	}
	return n;
}

static inline void plog_raw(const char *s)
{
	uint32_t n = plog_pos();
	while (*s && n < RAMOOPS_CONSOLE_SIZE - PRZ_HDR)
		*(volatile uint8_t *)(RAMOOPS_CONSOLE + PRZ_HDR + n++) = (uint8_t)*s++;
	plog_wr32(RAMOOPS_CONSOLE + 4, n);
	plog_wr32(RAMOOPS_CONSOLE + 8, n);
	__asm__ volatile("dsb sy" ::: "memory");
}

/* One line: TAG + text + " *XX\n". buf is assembled on the stack first. */
static inline void plog_line(const char *tag, const char *a, const char *b)
{
	char buf[160];
	unsigned n = 0, sum = 0;
	const char *parts[3] = { tag, a, b };
	for (int p = 0; p < 3; p++) {
		const char *s = parts[p];
		if (!s)
			continue;
		if (p && n < sizeof(buf) - 8)
			buf[n++] = ' ';
		while (*s && n < sizeof(buf) - 8)
			buf[n++] = *s++;
	}
	for (unsigned i = 0; i < n; i++)
		sum += (uint8_t)buf[i];
	static const char hx[] = "0123456789abcdef";
	buf[n++] = ' ';
	buf[n++] = '*';
	buf[n++] = hx[(sum >> 4) & 0xf];
	buf[n++] = hx[sum & 0xf];
	buf[n++] = '\n';
	buf[n] = 0;
	plog_raw(buf);
}

static inline void plog_fmt_hex(char *buf, uint64_t v)
{
	static const char hx[] = "0123456789abcdef";
	buf[0] = '0';
	buf[1] = 'x';
	for (int i = 0; i < 16; i++)
		buf[2 + i] = hx[(v >> (4 * (15 - i))) & 0xf];
	buf[18] = 0;
}

static inline void plog_hex(const char *tag, const char *label, uint64_t v)
{
	char h[19];
	plog_fmt_hex(h, v);
	char lab[96];
	unsigned n = 0;
	while (label[n] && n < sizeof(lab) - 21) {
		lab[n] = label[n];
		n++;
	}
	lab[n++] = ' ';
	for (int i = 0; i < 19; i++)
		lab[n + i] = h[i];
	plog_line(tag, lab, 0);
}
