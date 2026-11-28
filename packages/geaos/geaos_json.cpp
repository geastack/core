// SPDX-License-Identifier: Apache-2.0
#include "geaos_json.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace gea::geaos::json {

namespace {

constexpr int kMaxDepth = 32;

struct Parser {
	std::string_view text;
	std::size_t at = 0;
	std::string &error;

	bool fail(const char *what)
	{
		if (error.empty()) error = std::string(what) + " at offset " + std::to_string(at);
		return false;
	}

	void skipSpace()
	{
		while (at < text.size()) {
			const char c = text[at];
			if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
			at++;
		}
	}

	bool literal(const char *word)
	{
		const std::size_t n = std::strlen(word);
		if (text.substr(at, n) != word) return fail("invalid literal");
		at += n;
		return true;
	}

	static void appendUtf8(std::string &out, std::uint32_t cp)
	{
		if (cp < 0x80) {
			out += static_cast<char>(cp);
		} else if (cp < 0x800) {
			out += static_cast<char>(0xC0 | (cp >> 6));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		} else if (cp < 0x10000) {
			out += static_cast<char>(0xE0 | (cp >> 12));
			out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		} else {
			out += static_cast<char>(0xF0 | (cp >> 18));
			out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
			out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		}
	}

	bool hex4(std::uint32_t &out)
	{
		if (at + 4 > text.size()) return fail("truncated \\u escape");
		out = 0;
		for (int i = 0; i < 4; i++) {
			const char c = text[at++];
			out <<= 4;
			if (c >= '0' && c <= '9') out |= static_cast<std::uint32_t>(c - '0');
			else if (c >= 'a' && c <= 'f') out |= static_cast<std::uint32_t>(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') out |= static_cast<std::uint32_t>(c - 'A' + 10);
			else return fail("invalid \\u escape");
		}
		return true;
	}

	bool string(std::string &out)
	{
		at++;  // opening quote
		const std::size_t start = at;
		while (true) {
			if (at >= text.size()) return fail("unterminated string");
			const unsigned char c = static_cast<unsigned char>(text[at]);
			if (c == '"') break;
			if (c < 0x20) return fail("control character in string");
			if (c != '\\') {
				out += static_cast<char>(c);
				at++;
				continue;
			}
			at++;
			if (at >= text.size()) return fail("unterminated escape");
			const char e = text[at++];
			switch (e) {
			case '"': out += '"'; break;
			case '\\': out += '\\'; break;
			case '/': out += '/'; break;
			case 'b': out += '\b'; break;
			case 'f': out += '\f'; break;
			case 'n': out += '\n'; break;
			case 'r': out += '\r'; break;
			case 't': out += '\t'; break;
			case 'u': {
				std::uint32_t cp = 0;
				if (!hex4(cp)) return false;
				if (cp >= 0xD800 && cp <= 0xDBFF) {
					if (text.substr(at, 2) != "\\u") return fail("unpaired surrogate");
					at += 2;
					std::uint32_t low = 0;
					if (!hex4(low)) return false;
					if (low < 0xDC00 || low > 0xDFFF) return fail("unpaired surrogate");
					cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
				} else if (cp >= 0xDC00 && cp <= 0xDFFF) {
					return fail("unpaired surrogate");
				}
				if (cp == 0) return fail("NUL in string");
				appendUtf8(out, cp);
				break;
			}
			default: return fail("invalid escape");
			}
		}
		const std::string_view raw = text.substr(start, at - start);
		at++;  // closing quote
		if (!isValidUtf8(raw)) return fail("invalid UTF-8 in string");
		return true;
	}

	bool number(double &out)
	{
		const std::size_t start = at;
		if (at < text.size() && text[at] == '-') at++;
		if (at >= text.size()) return fail("invalid number");
		if (text[at] == '0') {
			at++;
		} else if (text[at] >= '1' && text[at] <= '9') {
			while (at < text.size() && text[at] >= '0' && text[at] <= '9') at++;
		} else {
			return fail("invalid number");
		}
		if (at < text.size() && text[at] == '.') {
			at++;
			const std::size_t digits = at;
			while (at < text.size() && text[at] >= '0' && text[at] <= '9') at++;
			if (at == digits) return fail("invalid number");
		}
		if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
			at++;
			if (at < text.size() && (text[at] == '+' || text[at] == '-')) at++;
			const std::size_t digits = at;
			while (at < text.size() && text[at] >= '0' && text[at] <= '9') at++;
			if (at == digits) return fail("invalid number");
		}
		const std::string copy(text.substr(start, at - start));
		out = std::strtod(copy.c_str(), nullptr);
		return true;
	}

	bool value(Value &out, int depth)
	{
		if (depth >= kMaxDepth) return fail("nesting too deep");
		skipSpace();
		if (at >= text.size()) return fail("unexpected end");
		const char c = text[at];
		if (c == '{') {
			out.kind = Value::Kind::Object;
			at++;
			skipSpace();
			if (at < text.size() && text[at] == '}') {
				at++;
				return true;
			}
			while (true) {
				skipSpace();
				if (at >= text.size() || text[at] != '"') return fail("expected key");
				std::string key;
				if (!string(key)) return false;
				for (const auto &member : out.members)
					if (member.first == key) return fail("duplicate key");
				skipSpace();
				if (at >= text.size() || text[at] != ':') return fail("expected ':'");
				at++;
				Value item;
				if (!value(item, depth + 1)) return false;
				out.members.emplace_back(std::move(key), std::move(item));
				skipSpace();
				if (at < text.size() && text[at] == ',') {
					at++;
					continue;
				}
				if (at < text.size() && text[at] == '}') {
					at++;
					return true;
				}
				return fail("expected ',' or '}'");
			}
		}
		if (c == '[') {
			out.kind = Value::Kind::Array;
			at++;
			skipSpace();
			if (at < text.size() && text[at] == ']') {
				at++;
				return true;
			}
			while (true) {
				Value item;
				if (!value(item, depth + 1)) return false;
				out.items.push_back(std::move(item));
				skipSpace();
				if (at < text.size() && text[at] == ',') {
					at++;
					continue;
				}
				if (at < text.size() && text[at] == ']') {
					at++;
					return true;
				}
				return fail("expected ',' or ']'");
			}
		}
		if (c == '"') {
			out.kind = Value::Kind::String;
			return string(out.string);
		}
		if (c == 't') {
			out.kind = Value::Kind::Bool;
			out.boolean = true;
			return literal("true");
		}
		if (c == 'f') {
			out.kind = Value::Kind::Bool;
			return literal("false");
		}
		if (c == 'n') {
			out.kind = Value::Kind::Null;
			return literal("null");
		}
		out.kind = Value::Kind::Number;
		return number(out.number);
	}
};

}  // namespace

const Value *Value::get(std::string_view key) const
{
	if (kind != Kind::Object) return nullptr;
	for (const auto &member : members)
		if (member.first == key) return &member.second;
	return nullptr;
}

bool parse(std::string_view text, Value &out, std::string &error)
{
	out = Value{};
	error.clear();
	// A UTF-8 byte-order mark is tolerated: some editors write one.
	if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
	Parser parser{text, 0, error};
	if (!parser.value(out, 0)) return false;
	parser.skipSpace();
	if (parser.at != text.size()) return parser.fail("trailing content");
	return true;
}

bool isValidUtf8(std::string_view text)
{
	std::size_t i = 0;
	while (i < text.size()) {
		const unsigned char c = static_cast<unsigned char>(text[i]);
		if (c < 0x80) {
			i++;
			continue;
		}
		std::size_t extra = 0;
		std::uint32_t cp = 0;
		if (c >= 0xC2 && c <= 0xDF) {
			extra = 1;
			cp = c & 0x1F;
		} else if (c >= 0xE0 && c <= 0xEF) {
			extra = 2;
			cp = c & 0x0F;
		} else if (c >= 0xF0 && c <= 0xF4) {
			extra = 3;
			cp = c & 0x07;
		} else {
			return false;
		}
		for (std::size_t k = 1; k <= extra; k++) {
			if (i + k >= text.size()) return false;
			const unsigned char d = static_cast<unsigned char>(text[i + k]);
			if ((d & 0xC0) != 0x80) return false;
			cp = (cp << 6) | (d & 0x3F);
		}
		if ((extra == 2 && cp < 0x800) || (extra == 3 && (cp < 0x10000 || cp > 0x10FFFF))) return false;
		if (cp >= 0xD800 && cp <= 0xDFFF) return false;
		i += extra + 1;
	}
	return true;
}

void appendQuoted(std::string &out, std::string_view text)
{
	static const char kHex[] = "0123456789abcdef";
	out += '"';
	for (const char ch : text) {
		const unsigned char c = static_cast<unsigned char>(ch);
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) {
				out += "\\u00";
				out += kHex[c >> 4];
				out += kHex[c & 0xF];
			} else {
				out += static_cast<char>(c);
			}
		}
	}
	out += '"';
}

}  // namespace gea::geaos::json
