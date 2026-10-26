/* Helpers shared by the olive shim and its test stub (header only, no relocations). */
#pragma once
#include <stdint.h>

#define OLIVE_LINE	608	/* BOOT_LINE_LENGTH */

/*
 * Boot-arg lookup in the payload's command line (no libc): returns 1 and
 * the value of KEY=VALUE (decimal or 0x hex; "KEY" alone = 1), else 0.
 */
static inline int cmdline_val(const char *line_, const char *key, uint64_t *val)
{
	for (int i = 0; i < OLIVE_LINE && line_[i]; i++) {
		if (i && line_[i - 1] != ' ')
			continue;
		int j = 0;
		while (key[j] && i + j < OLIVE_LINE && line_[i + j] == key[j])
			j++;
		if (key[j])
			continue;
		int k = i + j;
		if (k >= OLIVE_LINE || !line_[k] || line_[k] == ' ') {
			*val = 1;
			return 1;
		}
		if (line_[k] != '=')
			continue;
		k++;
		uint64_t v = 0;
		int base = 10;
		if (k + 1 < OLIVE_LINE && line_[k] == '0' && (line_[k + 1] == 'x' || line_[k + 1] == 'X')) {
			base = 16;
			k += 2;
		}
		for (; k < OLIVE_LINE && line_[k] && line_[k] != ' '; k++) {
			char c = line_[k];
			unsigned d;
			if (c >= '0' && c <= '9')
				d = c - '0';
			else if (base == 16 && c >= 'a' && c <= 'f')
				d = c - 'a' + 10;
			else if (base == 16 && c >= 'A' && c <= 'F')
				d = c - 'A' + 10;
			else
				break;
			v = v * base + d;
		}
		*val = v;
		return 1;
	}
	return 0;
}

/*
 * 64-bit FNV-1a over 8-byte words (zero-padded tail), read with aligned
 * 8-byte loads (MMU off = Device memory). olive/shim/test/mkstubpayload.py
 * computes the same value on the host.
 */
static inline uint64_t sum64(uintptr_t a, uint64_t n)
{
	volatile const uint64_t *w = (volatile const uint64_t *)a;
	uint64_t h = 0xcbf29ce484222325ULL;
	uint64_t words = n / 8;
	for (uint64_t i = 0; i < words; i++) {
		h ^= w[i];
		h *= 0x100000001b3ULL;
	}
	if (n & 7) {
		uint64_t t = w[words] & ((1ULL << (8 * (n & 7))) - 1);
		h ^= t;
		h *= 0x100000001b3ULL;
	}
	return h;
}

/* Bit count without libgcc: its __popcountdi2 uses SIMD (FP traps, CPACR_EL1.FPEN). */
static inline unsigned popcount32(uint32_t v)
{
	unsigned n = 0;
	for (; v; v &= v - 1)
		n++;
	return n;
}
