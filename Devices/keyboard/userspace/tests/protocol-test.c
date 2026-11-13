// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include "zsk-protocol.h"

struct result { unsigned int count, a, enter, down, left, right, up, releases, pad; };

static void decoded(void *ctx, const unsigned char *f, unsigned int len)
{
	struct result *r = ctx;
	r->count++;
	if (f[0] == 7) { assert(len == 21); r->pad++; return; }
	assert(len == 9);
	switch (f[3]) {
	case 0x04: r->a++; break;
	case 0x28: r->enter++; break;
	case 0x51: r->down++; break;
	case 0x50: r->left++; break;
	case 0x4f: r->right++; break;
	case 0x52: r->up++; break;
	case 0: r->releases++; break;
	default: break;
	}
}

static void feed(struct zsk_parser *p, struct result *r,
		 const unsigned char *bytes, size_t size)
{
	for (size_t i = 0; i < size; i++) zsk_parser_feed(p, bytes[i], decoded, r);
}

int main(int argc, char **argv)
{
	/* DOWN uses XOR 0x50, not a sum; split and coalesced reads must agree. */
	static const unsigned char stream[] = {
		1, 0, 5, 4, 0, 0, 0, 0, 0,
		1, 0, 1, 0, 0, 0, 0, 0, 0,
		1, 0, 0x50, 0x51, 0, 0, 0, 0, 0,
	};
	for (size_t split = 0; split <= sizeof(stream); split++) {
		struct zsk_parser p = {0}; struct result r = {0};
		feed(&p, &r, stream, split);
		feed(&p, &r, stream + split, sizeof(stream) - split);
		assert(r.count == 3 && r.a == 1 && r.releases == 1 && r.down == 1);
		assert(!p.used && !p.bad_checksums);
	}
	{
		struct zsk_parser p = {0}; struct result r = {0};
		unsigned char bad[] = {1,0,0xff,4,0,0,0,0,0,0xff,0xff};
		feed(&p, &r, bad, sizeof(bad));
		feed(&p, &r, stream, sizeof(stream));
		assert(r.count == 3 && r.a == 1 && r.down == 1);
		assert(p.bad_checksums == 1 && p.discarded == sizeof(bad));
	}
	{
		struct zsk_parser p = {0}; struct result r = {0};
		unsigned char pad[21] = {7,0,0,0,0,4,0,0,0,8,0,0,0,12};
		feed(&p, &r, pad, sizeof(pad));
		assert(r.pad == 1 && !p.used);
	}
	{
		struct zsk_parser p = {0}; struct result r = {0};
		/* Invalid contact identity must not be emitted as a pad report. */
		unsigned char badpad[21] = {7,0,0,0,0,0x84,0,0,0,8,0,0,0,12};
		feed(&p, &r, badpad, sizeof(badpad));
		feed(&p, &r, stream, sizeof(stream));
		assert(r.pad == 0 && r.count == 3 && p.bad_pad_reports == 1);
	}
	if (argc >= 2) {
		struct zsk_parser p = {0}; struct result r = {0};
		FILE *file = fopen(argv[1], "rb"); int b;
		if (!file) { perror("capture"); return 1; }
		while ((b = fgetc(file)) != EOF) zsk_parser_feed(&p, (unsigned char)b, decoded, &r);
		fclose(file);
		assert(r.count == 28 && r.a == 5 && r.enter == 4 && r.releases == 14);
		assert(r.up == 2 && r.down == 1 && r.left == 1 && r.right == 1);
		assert(!p.used && !p.bad_checksums && !p.discarded);
		printf("real_capture frames=%u A=%u ENTER=%u UP=%u DOWN=%u LEFT=%u RIGHT=%u releases=%u\n",
		       r.count,r.a,r.enter,r.up,r.down,r.left,r.right,r.releases);
	}
	if (argc == 3) {
		struct zsk_parser p = {0}; struct result r = {0};
		FILE *file = fopen(argv[2], "rb"); int b;
		if (!file) { perror("pad capture"); return 1; }
		while ((b = fgetc(file)) != EOF) zsk_parser_feed(&p, (unsigned char)b, decoded, &r);
		fclose(file);
		assert(r.pad == 387 && r.count == 387);
		assert(!p.used && !p.bad_checksums && !p.bad_pad_reports && !p.discarded);
		printf("real_touchpad_capture frames=%u\n", r.pad);
	}
	puts("PASS: split/coalesced reads, keyboard XOR, corruption recovery, 21-byte pad validation");
	return 0;
}
