// SPDX-License-Identifier: Apache-2.0
// The cryptography GeaOS package signing needs, and nothing more: SHA-512 and
// Ed25519 signature verification (signing happens on the developer's machine).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace gea::geaos::crypto {

class Sha512 {
public:
	Sha512();
	void update(const void *data, std::size_t length);
	void finish(std::uint8_t out[64]);

private:
	void block(const std::uint8_t *p);
	std::uint64_t state_[8];
	std::uint8_t buffer_[128];
	std::size_t used_ = 0;
	std::uint64_t total_ = 0;
};

void sha512(const void *data, std::size_t length, std::uint8_t out[64]);

// RFC 8032 Ed25519 (pure). Rejects a non-canonical scalar and a public key
// that is not a curve point.
bool ed25519Verify(const std::uint8_t signature[64], const void *message, std::size_t length, const std::uint8_t publicKey[32]);

// Signing, for tools and tests (devices only verify). `seed` is the 32-byte
// private key of RFC 8032.
void ed25519PublicKey(const std::uint8_t seed[32], std::uint8_t publicKey[32]);
void ed25519Sign(std::uint8_t signature[64], const void *message, std::size_t length, const std::uint8_t seed[32]);

std::string toBase64(const void *data, std::size_t length);
std::string toHex(const std::uint8_t *data, std::size_t length);
// Standard or URL-safe alphabet, padding optional. False on any other byte.
bool fromBase64(std::string_view text, std::string &out);

}  // namespace gea::geaos::crypto
