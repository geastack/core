// SPDX-License-Identifier: Apache-2.0
// SHA-512 and Ed25519 signature verification for GeaOS package signing.
// The field and group arithmetic is adapted from TweetNaCl (public domain,
// Bernstein, van Gastel, Janssen, Lange, Schwabe, Smetsers): small and
// constant-time, which is all a verifier on a microcontroller needs.
#include "geaos_crypto.h"

#include <cstring>

namespace gea::geaos::crypto {

// ---- SHA-512 -----------------------------------------------------------------

namespace {

constexpr std::uint64_t kK[80] = {
	0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL,
	0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
	0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL, 0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
	0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
	0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL, 0x983e5152ee66dfabULL,
	0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
	0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL,
	0x53380d139d95b3dfULL, 0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
	0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
	0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL, 0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
	0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL,
	0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
	0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL, 0xca273eceea26619cULL,
	0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
	0x113f9804bef90daeULL, 0x1b710b35131c471bULL, 0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
	0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

inline std::uint64_t rotr(std::uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

}  // namespace

Sha512::Sha512()
{
	state_[0] = 0x6a09e667f3bcc908ULL;
	state_[1] = 0xbb67ae8584caa73bULL;
	state_[2] = 0x3c6ef372fe94f82bULL;
	state_[3] = 0xa54ff53a5f1d36f1ULL;
	state_[4] = 0x510e527fade682d1ULL;
	state_[5] = 0x9b05688c2b3e6c1fULL;
	state_[6] = 0x1f83d9abfb41bd6bULL;
	state_[7] = 0x5be0cd19137e2179ULL;
}

void Sha512::block(const std::uint8_t *p)
{
	std::uint64_t w[80];
	for (int i = 0; i < 16; i++) {
		w[i] = 0;
		for (int j = 0; j < 8; j++) w[i] = (w[i] << 8) | p[i * 8 + j];
	}
	for (int i = 16; i < 80; i++) {
		const std::uint64_t s0 = rotr(w[i - 15], 1) ^ rotr(w[i - 15], 8) ^ (w[i - 15] >> 7);
		const std::uint64_t s1 = rotr(w[i - 2], 19) ^ rotr(w[i - 2], 61) ^ (w[i - 2] >> 6);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	std::uint64_t a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4], f = state_[5], g = state_[6], h = state_[7];
	for (int i = 0; i < 80; i++) {
		const std::uint64_t S1 = rotr(e, 14) ^ rotr(e, 18) ^ rotr(e, 41);
		const std::uint64_t ch = (e & f) ^ (~e & g);
		const std::uint64_t t1 = h + S1 + ch + kK[i] + w[i];
		const std::uint64_t S0 = rotr(a, 28) ^ rotr(a, 34) ^ rotr(a, 39);
		const std::uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
		const std::uint64_t t2 = S0 + maj;
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	state_[0] += a;
	state_[1] += b;
	state_[2] += c;
	state_[3] += d;
	state_[4] += e;
	state_[5] += f;
	state_[6] += g;
	state_[7] += h;
}

void Sha512::update(const void *data, std::size_t length)
{
	const auto *p = static_cast<const std::uint8_t *>(data);
	total_ += length;
	if (used_) {
		const std::size_t take = length < 128 - used_ ? length : 128 - used_;
		std::memcpy(buffer_ + used_, p, take);
		used_ += take;
		p += take;
		length -= take;
		if (used_ < 128) return;
		block(buffer_);
		used_ = 0;
	}
	while (length >= 128) {
		block(p);
		p += 128;
		length -= 128;
	}
	if (length) {
		std::memcpy(buffer_, p, length);
		used_ = length;
	}
}

void Sha512::finish(std::uint8_t out[64])
{
	const std::uint64_t bits = total_ * 8;
	const std::uint8_t pad = 0x80;
	update(&pad, 1);
	const std::uint8_t zero = 0;
	while (used_ != 112) update(&zero, 1);
	std::uint8_t length[16] = {};
	for (int i = 0; i < 8; i++) length[15 - i] = static_cast<std::uint8_t>(bits >> (8 * i));
	update(length, 16);
	for (int i = 0; i < 8; i++)
		for (int j = 0; j < 8; j++) out[i * 8 + j] = static_cast<std::uint8_t>(state_[i] >> (56 - 8 * j));
}

void sha512(const void *data, std::size_t length, std::uint8_t out[64])
{
	Sha512 h;
	h.update(data, length);
	h.finish(out);
}

// ---- Ed25519 verification ----------------------------------------------------

namespace {

using i64 = std::int64_t;
using gf = i64[16];

const gf gf0 = {0};
const gf gf1 = {1};
const gf D2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
               0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
const gf Dc = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
               0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203};
const gf Xb = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
               0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
const gf Yb = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
               0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
const gf Ic = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
               0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83};
const i64 Lc[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
                    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10};

int verify32(const std::uint8_t *x, const std::uint8_t *y)
{
	std::uint32_t d = 0;
	for (int i = 0; i < 32; i++) d |= x[i] ^ y[i];
	return (1 & ((d - 1) >> 8)) - 1;
}

void set25519(gf r, const gf a)
{
	for (int i = 0; i < 16; i++) r[i] = a[i];
}

void car25519(gf o)
{
	for (int i = 0; i < 16; i++) {
		o[i] += (i64(1) << 16);
		const i64 c = o[i] >> 16;
		o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
		o[i] -= c * (i64(1) << 16);
	}
}

void sel25519(gf p, gf q, int b)
{
	const i64 c = ~(i64(b) - 1);
	for (int i = 0; i < 16; i++) {
		const i64 t = c & (p[i] ^ q[i]);
		p[i] ^= t;
		q[i] ^= t;
	}
}

void pack25519(std::uint8_t *o, const gf n)
{
	gf m, t;
	for (int i = 0; i < 16; i++) t[i] = n[i];
	car25519(t);
	car25519(t);
	car25519(t);
	for (int j = 0; j < 2; j++) {
		m[0] = t[0] - 0xffed;
		for (int i = 1; i < 15; i++) {
			m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
			m[i - 1] &= 0xffff;
		}
		m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
		const int b = static_cast<int>((m[15] >> 16) & 1);
		m[14] &= 0xffff;
		sel25519(t, m, 1 - b);
	}
	for (int i = 0; i < 16; i++) {
		o[2 * i] = static_cast<std::uint8_t>(t[i] & 0xff);
		o[2 * i + 1] = static_cast<std::uint8_t>(t[i] >> 8);
	}
}

int neq25519(const gf a, const gf b)
{
	std::uint8_t c[32], d[32];
	pack25519(c, a);
	pack25519(d, b);
	return verify32(c, d);
}

std::uint8_t par25519(const gf a)
{
	std::uint8_t d[32];
	pack25519(d, a);
	return d[0] & 1;
}

void unpack25519(gf o, const std::uint8_t *n)
{
	for (int i = 0; i < 16; i++) o[i] = n[2 * i] + (i64(n[2 * i + 1]) << 8);
	o[15] &= 0x7fff;
}

void A(gf o, const gf a, const gf b)
{
	for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

void Z(gf o, const gf a, const gf b)
{
	for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

void M(gf o, const gf a, const gf b)
{
	i64 t[31] = {0};
	for (int i = 0; i < 16; i++)
		for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
	for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
	for (int i = 0; i < 16; i++) o[i] = t[i];
	car25519(o);
	car25519(o);
}

void S(gf o, const gf a) { M(o, a, a); }

void inv25519(gf o, const gf in)
{
	gf c;
	for (int a = 0; a < 16; a++) c[a] = in[a];
	for (int a = 253; a >= 0; a--) {
		S(c, c);
		if (a != 2 && a != 4) M(c, c, in);
	}
	for (int a = 0; a < 16; a++) o[a] = c[a];
}

void pow2523(gf o, const gf in)
{
	gf c;
	for (int a = 0; a < 16; a++) c[a] = in[a];
	for (int a = 250; a >= 0; a--) {
		S(c, c);
		if (a != 1) M(c, c, in);
	}
	for (int a = 0; a < 16; a++) o[a] = c[a];
}

void add(gf p[4], gf q[4])
{
	gf a, b, c, d, t, e, f, g, h;
	Z(a, p[1], p[0]);
	Z(t, q[1], q[0]);
	M(a, a, t);
	A(b, p[0], p[1]);
	A(t, q[0], q[1]);
	M(b, b, t);
	M(c, p[3], q[3]);
	M(c, c, D2);
	M(d, p[2], q[2]);
	A(d, d, d);
	Z(e, b, a);
	Z(f, d, c);
	A(g, d, c);
	A(h, b, a);
	M(p[0], e, f);
	M(p[1], h, g);
	M(p[2], g, f);
	M(p[3], e, h);
}

void cswap(gf p[4], gf q[4], std::uint8_t b)
{
	for (int i = 0; i < 4; i++) sel25519(p[i], q[i], b);
}

void pack(std::uint8_t *r, gf p[4])
{
	gf tx, ty, zi;
	inv25519(zi, p[2]);
	M(tx, p[0], zi);
	M(ty, p[1], zi);
	pack25519(r, ty);
	r[31] ^= static_cast<std::uint8_t>(par25519(tx) << 7);
}

void scalarmult(gf p[4], gf q[4], const std::uint8_t *s)
{
	set25519(p[0], gf0);
	set25519(p[1], gf1);
	set25519(p[2], gf1);
	set25519(p[3], gf0);
	for (int i = 255; i >= 0; --i) {
		const std::uint8_t b = (s[i / 8] >> (i & 7)) & 1;
		cswap(p, q, b);
		add(q, p);
		add(p, p);
		cswap(p, q, b);
	}
}

void scalarbase(gf p[4], const std::uint8_t *s)
{
	gf q[4];
	set25519(q[0], Xb);
	set25519(q[1], Yb);
	set25519(q[2], gf1);
	M(q[3], Xb, Yb);
	scalarmult(p, q, s);
}

void modL(std::uint8_t *r, i64 x[64])
{
	i64 carry;
	for (int i = 63; i >= 32; --i) {
		carry = 0;
		int j;
		for (j = i - 32; j < i - 12; ++j) {
			x[j] += carry - 16 * x[i] * Lc[j - (i - 32)];
			carry = (x[j] + 128) >> 8;
			x[j] -= carry * 256;
		}
		x[j] += carry;
		x[i] = 0;
	}
	carry = 0;
	for (int j = 0; j < 32; j++) {
		x[j] += carry - (x[31] >> 4) * Lc[j];
		carry = x[j] >> 8;
		x[j] &= 255;
	}
	for (int j = 0; j < 32; j++) x[j] -= carry * Lc[j];
	for (int i = 0; i < 32; i++) {
		x[i + 1] += x[i] >> 8;
		r[i] = static_cast<std::uint8_t>(x[i] & 255);
	}
}

void reduce(std::uint8_t *r)
{
	i64 x[64];
	for (int i = 0; i < 64; i++) x[i] = r[i];
	for (int i = 0; i < 64; i++) r[i] = 0;
	modL(r, x);
}

int unpackneg(gf r[4], const std::uint8_t p[32])
{
	gf t, chk, num, den, den2, den4, den6;
	set25519(r[2], gf1);
	unpack25519(r[1], p);
	S(num, r[1]);
	M(den, num, Dc);
	Z(num, num, r[2]);
	A(den, r[2], den);
	S(den2, den);
	S(den4, den2);
	M(den6, den4, den2);
	M(t, den6, num);
	M(t, t, den);
	pow2523(t, t);
	M(t, t, num);
	M(t, t, den);
	M(t, t, den);
	M(r[0], t, den);
	S(chk, r[0]);
	M(chk, chk, den);
	if (neq25519(chk, num)) M(r[0], r[0], Ic);
	S(chk, r[0]);
	M(chk, chk, den);
	if (neq25519(chk, num)) return -1;
	if (par25519(r[0]) == (p[31] >> 7)) Z(r[0], gf0, r[0]);
	M(r[3], r[0], r[1]);
	return 0;
}

// s < L: a signature with a larger scalar is a malleated copy of a valid one.
bool scalarIsCanonical(const std::uint8_t s[32])
{
	for (int i = 31; i >= 0; i--) {
		if (s[i] < Lc[i]) return true;
		if (s[i] > Lc[i]) return false;
	}
	return false;  // s == L
}

}  // namespace

bool ed25519Verify(const std::uint8_t signature[64], const void *message, std::size_t length, const std::uint8_t publicKey[32])
{
	gf p[4], q[4];
	if (!scalarIsCanonical(signature + 32)) return false;
	if (unpackneg(q, publicKey)) return false;
	std::uint8_t h[64];
	Sha512 hash;
	hash.update(signature, 32);
	hash.update(publicKey, 32);
	hash.update(message, length);
	hash.finish(h);
	reduce(h);
	scalarmult(p, q, h);
	scalarbase(q, signature + 32);
	add(p, q);
	std::uint8_t t[32];
	pack(t, p);
	return verify32(signature, t) == 0;
}

void ed25519PublicKey(const std::uint8_t seed[32], std::uint8_t publicKey[32])
{
	std::uint8_t d[64];
	sha512(seed, 32, d);
	d[0] &= 248;
	d[31] &= 127;
	d[31] |= 64;
	gf p[4];
	scalarbase(p, d);
	pack(publicKey, p);
}

void ed25519Sign(std::uint8_t signature[64], const void *message, std::size_t length, const std::uint8_t seed[32])
{
	std::uint8_t d[64], r[64], h[64], publicKey[32];
	sha512(seed, 32, d);
	d[0] &= 248;
	d[31] &= 127;
	d[31] |= 64;
	ed25519PublicKey(seed, publicKey);
	Sha512 nonce;
	nonce.update(d + 32, 32);
	nonce.update(message, length);
	nonce.finish(r);
	reduce(r);
	gf p[4];
	scalarbase(p, r);
	pack(signature, p);
	Sha512 challenge;
	challenge.update(signature, 32);
	challenge.update(publicKey, 32);
	challenge.update(message, length);
	challenge.finish(h);
	reduce(h);
	i64 x[64] = {0};
	for (int i = 0; i < 32; i++) x[i] = r[i];
	for (int i = 0; i < 32; i++)
		for (int j = 0; j < 32; j++) x[i + j] += h[i] * i64(d[j]);
	modL(signature + 32, x);
}

std::string toBase64(const void *data, std::size_t length)
{
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	const auto *p = static_cast<const std::uint8_t *>(data);
	std::string out;
	std::size_t i = 0;
	for (; i + 2 < length; i += 3) {
		const std::uint32_t v = (p[i] << 16) | (p[i + 1] << 8) | p[i + 2];
		out += alphabet[v >> 18];
		out += alphabet[(v >> 12) & 63];
		out += alphabet[(v >> 6) & 63];
		out += alphabet[v & 63];
	}
	if (i < length) {
		const std::uint32_t v = (p[i] << 16) | (i + 1 < length ? p[i + 1] << 8 : 0);
		out += alphabet[v >> 18];
		out += alphabet[(v >> 12) & 63];
		out += i + 1 < length ? alphabet[(v >> 6) & 63] : '=';
		out += '=';
	}
	return out;
}

// ---- Encodings ---------------------------------------------------------------

std::string toHex(const std::uint8_t *data, std::size_t length)
{
	static const char digits[] = "0123456789abcdef";
	std::string out;
	out.reserve(length * 2);
	for (std::size_t i = 0; i < length; i++) {
		out += digits[data[i] >> 4];
		out += digits[data[i] & 15];
	}
	return out;
}

bool fromBase64(std::string_view text, std::string &out)
{
	auto value = [](char c) -> int {
		if (c >= 'A' && c <= 'Z') return c - 'A';
		if (c >= 'a' && c <= 'z') return c - 'a' + 26;
		if (c >= '0' && c <= '9') return c - '0' + 52;
		if (c == '+' || c == '-') return 62;
		if (c == '/' || c == '_') return 63;
		return -1;
	};
	out.clear();
	std::uint32_t buffer = 0;
	int bits = 0;
	std::size_t i = 0;
	for (; i < text.size() && text[i] != '='; i++) {
		const int v = value(text[i]);
		if (v < 0) return false;
		buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out += static_cast<char>((buffer >> bits) & 0xff);
		}
	}
	for (; i < text.size(); i++)
		if (text[i] != '=') return false;
	return bits < 6;
}

}  // namespace gea::geaos::crypto
