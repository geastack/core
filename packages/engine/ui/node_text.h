// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "storage_usage.h"
#include <cstdint>
#include <string>

namespace gea::embedded::ui
{
// True when CSS white-space collapsing would change `text`: a tab, newline,
// carriage return or form feed, or two spaces in a row. The answer is kept
// with the text when it is assigned, so measuring and painting read a bit
// instead of scanning the text twice per call (about 389k scans in one run
// of the desktop CSS suite, 40 us a frame of a 24-paragraph scene).
inline bool hasCollapsibleWhitespace(const char *text)
{
	if (!text) return false;
	for (const char *p = text; *p; ++p) {
		const char c = *p;
		if (c == '\t' || c == '\n' || c == '\r' || c == '\f') return true;
		if (c == ' ' && p[1] == ' ') return true;
	}
	return false;
}

// Empty nodes own no string storage. Nonempty text lives in stable pooled
// records, preserving c_str() addresses while other nodes are created/removed.
// Copies own independent records: retained display commands can scrub one
// node's old buffer without invalidating a clone's unchanged text.
class NodeText
{
  public:
	NodeText() = default;
	NodeText(const NodeText &other);
	NodeText(NodeText &&other) noexcept;
	~NodeText();
	NodeText &operator=(const NodeText &other);
	NodeText &operator=(NodeText &&other) noexcept;
	NodeText &operator=(const std::string &text)
	{
		assign(text.c_str());
		return *this;
	}
	NodeText &operator=(const char *text)
	{
		assign(text);
		return *this;
	}
	// Pool allocation payload (including spare slots/map), excluding string
	// character buffers and allocator metadata. Useful alongside heap census.
	static std::size_t storageBytes();
	static StorageUsage storageUsage();
	void clear();
	void assign(const char *text);
	bool empty() const { return handle_ == kEmpty; }
	// hasCollapsibleWhitespace() of the text, computed by assign().
	bool collapsible() const { return handle_ != kEmpty && (handle_ & kCollapsible) != 0; }
	const std::string &str() const;
	operator const std::string &() const { return str(); }
	const char *c_str() const { return str().c_str(); }
	const char *data() const { return c_str(); }
	std::size_t size() const { return str().size(); }
	char operator[](std::size_t i) const { return str()[i]; }
	std::size_t find(char c) const { return str().find(c); }
	std::size_t find_first_not_of(const char *chars) const
	{
		return str().find_first_not_of(chars);
	}
	std::string substr(std::size_t pos,
					   std::size_t count = std::string::npos) const
	{
		return str().substr(pos, count);
	}
	friend bool operator==(const NodeText &a, const NodeText &b)
	{
		return a.str() == b.str();
	}
	friend bool operator==(const NodeText &a, const std::string &b)
	{
		return a.str() == b;
	}
	friend bool operator==(const NodeText &a, const char *b)
	{
		return a.str() == b;
	}
	friend bool operator!=(const NodeText &a, const NodeText &b)
	{
		return !(a == b);
	}
	friend bool operator!=(const NodeText &a, const std::string &b)
	{
		return !(a == b);
	}
	friend bool operator!=(const NodeText &a, const char *b)
	{
		return !(a == b);
	}

  private:
	static constexpr std::uint32_t kEmpty = UINT32_MAX;
	// The pool index in the low bits; the top bit is collapsible(), so the
	// node stays four bytes. kEmpty has every bit set and is tested whole.
	static constexpr std::uint32_t kCollapsible = 0x80000000u;
	std::uint32_t index() const { return handle_ & ~kCollapsible; }
	std::uint32_t handle_ = kEmpty;
};
} // namespace gea::embedded::ui
