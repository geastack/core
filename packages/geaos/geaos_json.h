// SPDX-License-Identifier: Apache-2.0
// A strict, bounded JSON reader for GeaOS manifests (RFC 8259). Internal to
// the geaos package: targets differ in which JSON library they ship, and a
// manifest comes from untrusted storage, so it is parsed here with limits on
// depth and size rather than by whatever the platform provides.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gea::geaos::json {

struct Value {
	enum class Kind { Null, Bool, Number, String, Array, Object } kind = Kind::Null;
	bool boolean = false;
	double number = 0;
	std::string string;
	std::vector<Value> items;
	std::vector<std::pair<std::string, Value>> members;

	// The member named `key`, or nullptr (also for a non-object).
	const Value *get(std::string_view key) const;
};

// Parses one JSON document. Duplicate object keys, invalid UTF-8, nesting
// deeper than 32 and trailing content are errors.
bool parse(std::string_view text, Value &out, std::string &error);

// True if `text` is well-formed UTF-8 with no overlong forms or surrogates.
bool isValidUtf8(std::string_view text);

// Appends `text` as a JSON string literal.
void appendQuoted(std::string &out, std::string_view text);

}  // namespace gea::geaos::json
