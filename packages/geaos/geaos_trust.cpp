// SPDX-License-Identifier: Apache-2.0
// GeaOS package signatures and the trust policy they are checked against.
#include "geaos.h"
#include "geaos_crypto.h"
#include "geaos_json.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <mutex>

namespace gea::geaos {

namespace {

std::mutex &policyMutex()
{
	static std::mutex mutex;
	return mutex;
}

TrustPolicy &policyStorage()
{
	static TrustPolicy policy;
	return policy;
}

bool decodeKey(const std::string &text, std::size_t bytes, std::string &out)
{
	return crypto::fromBase64(text, out) && out.size() == bytes;
}

bool contains(const std::vector<std::string> &list, const std::string &value)
{
	return std::find(list.begin(), list.end(), value) != list.end();
}

}  // namespace

const char *trustName(Trust trust)
{
	switch (trust) {
	case Trust::Verified: return "verified";
	case Trust::Unverified: return "unverified";
	case Trust::Unsigned: return "unsigned";
	}
	return "unsigned";
}

void setTrustPolicy(TrustPolicy policy)
{
	std::lock_guard<std::mutex> lock(policyMutex());
	policyStorage() = std::move(policy);
}

TrustPolicy trustPolicy()
{
	std::lock_guard<std::mutex> lock(policyMutex());
	return policyStorage();
}

std::string signingMessage(std::vector<std::tuple<std::string, std::uint64_t, std::string>> entries)
{
	std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) { return std::get<0>(a) < std::get<0>(b); });
	std::string message = "gea-package-v1\n";
	for (const auto &[name, size, digest] : entries) {
		if (name == kSignatureEntry) continue;
		message += name;
		message += '\t';
		message += std::to_string(size);
		message += '\t';
		message += digest;
		message += '\n';
	}
	return message;
}

std::string certificateMessage(std::string_view developer, std::string_view key, std::int64_t expires)
{
	std::string message = "gea-developer-v1\n";
	message.append(developer);
	message += '\n';
	message.append(key);
	message += '\n';
	message += std::to_string(expires);
	message += '\n';
	return message;
}

bool checkSignature(std::string_view signatureJson, const std::string &message, const TrustPolicy &policy, SignatureCheck &out,
                    std::string &error)
{
	out = SignatureCheck{};
	json::Value root;
	std::string parseError;
	if (!json::parse(signatureJson, root, parseError) || root.kind != json::Value::Kind::Object) {
		error = "signature.json is damaged" + (parseError.empty() ? std::string() : ": " + parseError);
		return false;
	}
	const auto text = [&](const json::Value *v) { return v && v->kind == json::Value::Kind::String ? v->string : std::string(); };
	const std::string developer = text(root.get("developer"));
	const std::string keyText = text(root.get("key"));
	std::string key, signature;
	if (developer.empty() || !decodeKey(keyText, 32, key)) {
		error = "signature.json has no valid developer and key";
		return false;
	}
	out.developer = developer;
	if (contains(policy.revoked, developer) || contains(policy.revoked, keyText)) {
		out.revoked = true;
		error = "the developer \"" + developer + "\" has been revoked";
		return false;
	}
	if (!message.empty()) {
		if (!decodeKey(text(root.get("signature")), 64, signature) ||
		    !crypto::ed25519Verify(reinterpret_cast<const std::uint8_t *>(signature.data()), message.data(), message.size(),
		                           reinterpret_cast<const std::uint8_t *>(key.data()))) {
			error = "the signature does not match the package: it was changed after signing, or signed for another app or target";
			return false;
		}
	}
	out.trust = Trust::Unverified;
	const json::Value *certificate = root.get("certificate");
	if (!certificate || certificate->kind != json::Value::Kind::Object) return true;  // self-signed
	const json::Value *expiresValue = certificate->get("expires");
	std::string certSignature;
	if (!expiresValue || expiresValue->kind != json::Value::Kind::Number || !std::isfinite(expiresValue->number) ||
	    !decodeKey(text(certificate->get("signature")), 64, certSignature))
		return true;  // a certificate no root could have signed: unverified, not refused
	const auto expires = static_cast<std::int64_t>(expiresValue->number);
	const std::string vouched = certificateMessage(developer, keyText, expires);
	for (const std::string &rootText : policy.roots) {
		std::string rootKey;
		if (!decodeKey(rootText, 32, rootKey)) continue;
		if (!crypto::ed25519Verify(reinterpret_cast<const std::uint8_t *>(certSignature.data()), vouched.data(), vouched.size(),
		                           reinterpret_cast<const std::uint8_t *>(rootKey.data())))
			continue;
		std::int64_t now = policy.now;
		if (now < 0) {
			now = static_cast<std::int64_t>(std::time(nullptr));
			if (now < 1700000000) now = 0;  // a clock that was never set
		}
		if (now > 0 && expires > 0 && now > expires) {
			error = "the certificate of developer \"" + developer + "\" has expired";
			return false;
		}
		out.trust = Trust::Verified;
		return true;
	}
	return true;
}

}  // namespace gea::geaos
