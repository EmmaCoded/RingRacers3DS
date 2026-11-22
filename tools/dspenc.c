// Ring Racers 3DS : DSP-ADPCM encoder (native format of the 3DS's DSP).
// Usage: dspenc input.raw output.bin [loop_sample]
//   input: PCM16 mono little-endian
//   output : u16 coefs[16] | u32 samples | u32 loop (aligned to 14) |
// u16 ps_loop | h1_loop | h2_loop | u16 0 | 8-byte frames
// Frame: 1 byte (coef_index<<4 | scale) + 14 signed nibbles.
// Decoding (what the DSP does):
//   s = clamp16(((nib << scale) << 11) + 1024 + c1*h1 + c2*h2) >> 11)
// Coefficients: 8 order-2 predictor pairs chosen by k-means over
// the optimal predictor for each frame. Each frame tries the 8 pairs and the 12
// scales and keeps the one with the lowest error (using the real history).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define FS 14

static inline int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

static void lpc2(const int16_t *x, int n, int h1, int h2, double *a1, double *a2, double *w)
{
	// least squares: x[i] ~ a1*x[i-1] + a2*x[i-2]
	double r11 = 0, r12 = 0, r22 = 0, r01 = 0, r02 = 0, e = 0;
	int p1 = h1, p2 = h2;
	for (int i = 0; i < n; i++)
	{
		double s = x[i];
		r11 += (double)p1 * p1; r12 += (double)p1 * p2; r22 += (double)p2 * p2;
		r01 += s * p1; r02 += s * p2; e += s * s;
		p2 = p1; p1 = x[i];
	}
	double det = r11 * r22 - r12 * r12;
	if (fabs(det) < 1e-6) { *a1 = r11 > 1e-6 ? r01 / r11 : 0; *a2 = 0; }
	else { *a1 = (r01 * r22 - r02 * r12) / det; *a2 = (r02 * r11 - r01 * r12) / det; }
	if (*a1 > 15.99) *a1 = 15.99; if (*a1 < -16) *a1 = -16;
	if (*a2 > 15.99) *a2 = 15.99; if (*a2 < -16) *a2 = -16;
	*w = sqrt(e) + 1.0;
}

// Encodes a frame with a coef pair; returns the error and writes bytes/history.
static double enc_frame(const int16_t *x, int n, int c1, int c2, int sc, int *h1, int *h2, uint8_t *nib)
{
	int a = *h1, b = *h2;
	double err = 0;
	for (int i = 0; i < FS; i++)
	{
		int s = i < n ? x[i] : 0;
		int pred = c1 * a + c2 * b;
		int diff = (s << 11) - pred - 1024;
		// nibble = round(diff / (2^(scale+11)))
		double q = (double)diff / (double)(1 << (sc + 11));
		int nb = (int)lround(q);
		if (nb > 7) nb = 7; if (nb < -8) nb = -8;
		int out = clamp16((((nb << sc) << 11) + 1024 + pred) >> 11);
		if (i < n) { double d = (double)(s - out); err += d * d; }
		nib[i] = (uint8_t)(nb & 15);
		b = a; a = out;
	}
	*h1 = a; *h2 = b;
	return err;
}

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: dspenc in.raw out.bin [loop]\n"); return 1; }
	FILE *f = fopen(argv[1], "rb");
	if (!f) return 1;
	fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
	int n = (int)(len / 2);
	int16_t *x = (int16_t *)calloc((size_t)n + FS, 2);
	if (fread(x, 2, (size_t)n, f) != (size_t)n) return 1;
	fclose(f);
	uint32_t loop = argc > 3 ? (uint32_t)strtoul(argv[3], 0, 10) : 0;
	loop -= loop % FS;
	if (loop >= (uint32_t)n) loop = 0;

	int nf = (n + FS - 1) / FS;
	if (nf < 1) nf = 1;
	// 1) optimal predictor per frame
	double *A1 = malloc(sizeof(double) * nf), *A2 = malloc(sizeof(double) * nf), *W = malloc(sizeof(double) * nf);
	for (int k = 0; k < nf; k++)
	{
		int st = k * FS, m = n - st < FS ? n - st : FS;
		if (m < 0) m = 0;
		int h1 = st > 0 ? x[st - 1] : 0, h2 = st > 1 ? x[st - 2] : 0;
		lpc2(x + st, m, h1, h2, &A1[k], &A2[k], &W[k]);
	}
	// 2) k-means with 8 pairs (start: typical pairs)
	double C1[8] = { 0, 1, 2, 1.8, 1.5, 0.9, 1.95, 0.5 }, C2[8] = { 0, 0, -1, -0.85, -0.6, -0.2, -0.97, 0 };
	for (int it = 0; it < 16; it++)
	{
		double s1[8] = {0}, s2[8] = {0}, sw[8] = {0};
		for (int k = 0; k < nf; k++)
		{
			int best = 0; double bd = 1e30;
			for (int c = 0; c < 8; c++)
			{
				double d = (A1[k] - C1[c]) * (A1[k] - C1[c]) + (A2[k] - C2[c]) * (A2[k] - C2[c]);
				if (d < bd) { bd = d; best = c; }
			}
			s1[best] += A1[k] * W[k]; s2[best] += A2[k] * W[k]; sw[best] += W[k];
		}
		for (int c = 1; c < 8; c++) // pair 0 stays at (0,0): silence/noise
			if (sw[c] > 0) { C1[c] = s1[c] / sw[c]; C2[c] = s2[c] / sw[c]; }
	}
	int16_t coef[16];
	for (int c = 0; c < 8; c++)
	{
		coef[c * 2] = (int16_t)clamp16((int)lround(C1[c] * 2048));
		coef[c * 2 + 1] = (int16_t)clamp16((int)lround(C2[c] * 2048));
	}
	// 3) encode
	uint8_t *out = calloc((size_t)nf, 8);
	int h1 = 0, h2 = 0;
	uint16_t lps = 0; int16_t lh1 = 0, lh2 = 0;
	uint8_t nib[FS], bnib[FS];
	for (int k = 0; k < nf; k++)
	{
		int st = k * FS, m = n - st < FS ? n - st : FS;
		double be = 1e300; int bc = 0, bs = 0, bh1 = 0, bh2 = 0;
		for (int c = 0; c < 8; c++)
		{
			// approximate scale: maximum residual with the real history
			int a0 = h1, b0 = h2, mx = 0;
			for (int i = 0; i < m; i++)
			{
				int r = x[st + i] - ((coef[c * 2] * a0 + coef[c * 2 + 1] * b0 + 1024) >> 11);
				if (r < 0) r = -r;
				if (r > mx) mx = r;
				b0 = a0; a0 = x[st + i];
			}
			int s0 = 0;
			while (s0 < 11 && (mx >> s0) > 7) s0++;
			for (int sc = s0 > 0 ? s0 - 1 : 0; sc <= s0 + 1 && sc < 12; sc++)
			{
				int a = h1, b = h2;
				double e = enc_frame(x + st, m, coef[c * 2], coef[c * 2 + 1], sc, &a, &b, nib);
				if (e < be) { be = e; bc = c; bs = sc; bh1 = a; bh2 = b; memcpy(bnib, nib, FS); }
			}
		}
		if ((uint32_t)st == loop) { lps = (uint16_t)((bc << 4) | bs); lh1 = (int16_t)h1; lh2 = (int16_t)h2; }
		uint8_t *o = out + (size_t)k * 8;
		o[0] = (uint8_t)((bc << 4) | bs);
		for (int i = 0; i < FS; i += 2) o[1 + i / 2] = (uint8_t)((bnib[i] << 4) | bnib[i + 1]);
		h1 = bh1; h2 = bh2;
	}
	FILE *g = fopen(argv[2], "wb");
	if (!g) return 1;
	fwrite(coef, 2, 16, g);
	uint32_t hdr[2] = { (uint32_t)n, loop };
	fwrite(hdr, 4, 2, g);
	uint16_t z = 0;
	fwrite(&lps, 2, 1, g); fwrite(&lh1, 2, 1, g); fwrite(&lh2, 2, 1, g); fwrite(&z, 2, 1, g);
	fwrite(out, 8, (size_t)nf, g);
	fclose(g);
	return 0;
}
