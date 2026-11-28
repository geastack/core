// SPDX-License-Identifier: Apache-2.0
#include "geaos.h"

#include <algorithm>
#include <cstring>

namespace gea::geaos {

namespace {

constexpr char kMagic[8] = {'G', 'E', 'A', 'P', 'K', 'G', 0x1a, 0};

std::uint16_t u16(const unsigned char *p)
{
	return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t u32(const unsigned char *p)
{
	return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
	       (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void put16(std::string &out, std::uint16_t v)
{
	out += static_cast<char>(v & 0xFF);
	out += static_cast<char>(v >> 8);
}

void put32(std::string &out, std::uint32_t v)
{
	for (int i = 0; i < 4; i++) out += static_cast<char>((v >> (8 * i)) & 0xFF);
}

}  // namespace

std::uint32_t crc32(const void *data, std::size_t length, std::uint32_t crc)
{
	struct Table {
		std::uint32_t entries[256];
		Table()
		{
			for (std::uint32_t i = 0; i < 256; i++) {
				std::uint32_t c = i;
				for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
				entries[i] = c;
			}
		}
	};
	static const Table tableHolder;  // initialized once, thread-safe
	const std::uint32_t *table = tableHolder.entries;
	crc = ~crc;
	const auto *p = static_cast<const unsigned char *>(data);
	for (std::size_t i = 0; i < length; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

bool isSafeEntryName(std::string_view name)
{
	if (name.empty() || name.size() > kMaxPathLength) return false;
	if (name.front() == '/') return false;
	for (const char c : name)
		if (c == '\0' || c == '\\' || static_cast<unsigned char>(c) < 0x20) return false;
	std::size_t start = 0;
	while (start <= name.size()) {
		const std::size_t slash = name.find('/', start);
		const std::string_view part = name.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
		if (part.empty() || part == "." || part == "..") return false;
		if (slash == std::string_view::npos) break;
		start = slash + 1;
	}
	return true;
}

std::string programEntryName(std::string_view targetKey)
{
	return "program/" + std::string(targetKey);
}

bool PackageReader::open(std::unique_ptr<File> file, std::string &error)
{
	file_.reset();
	entries_.clear();
	if (!file) {
		error = "cannot open package";
		return false;
	}
	const std::uint64_t fileSize = file->size();
	unsigned char header[kPackageHeaderSize] = {};
	if (fileSize < sizeof(kMagic) || !file->readAt(0, header, sizeof(kMagic)) || std::memcmp(header, kMagic, sizeof(kMagic)) != 0) {
		error = "not a .geapp package";
		return false;
	}
	if (fileSize < kPackageHeaderSize || !file->readAt(0, header, sizeof(header))) {
		error = "package is truncated";
		return false;
	}
	const std::uint16_t format = u16(header + 8);
	if (format != kPackageFormat) {
		error = "unsupported package format " + std::to_string(format);
		return false;
	}
	const std::uint16_t count = u16(header + 10);
	const std::uint32_t indexOffset = u32(header + 12);
	const std::uint32_t indexSize = u32(header + 16);
	const std::uint32_t indexCrc = u32(header + 20);
	for (std::size_t i = 24; i < kPackageHeaderSize; i++) {
		if (header[i] != 0) {
			error = "package header has non-zero reserved bytes";
			return false;
		}
	}
	if (count > kMaxPackageEntries) {
		error = "package has too many entries";
		return false;
	}
	if (indexOffset < kPackageHeaderSize || static_cast<std::uint64_t>(indexOffset) + indexSize > fileSize ||
	    indexSize > count * (2u + kMaxPathLength + 12u)) {
		error = "package index is out of bounds";
		return false;
	}
	std::string index(indexSize, '\0');
	if (indexSize && !file->readAt(indexOffset, index.data(), indexSize)) {
		error = "package is truncated";
		return false;
	}
	if (crc32(index.data(), index.size()) != indexCrc) {
		error = "package index is corrupt";
		return false;
	}

	std::vector<PackageEntry> entries;
	entries.reserve(count);
	const auto *p = reinterpret_cast<const unsigned char *>(index.data());
	std::size_t at = 0;
	for (std::uint16_t i = 0; i < count; i++) {
		if (at + 2 > index.size()) {
			error = "package index is truncated";
			return false;
		}
		const std::uint16_t nameLength = u16(p + at);
		at += 2;
		if (at + nameLength + 12 > index.size()) {
			error = "package index is truncated";
			return false;
		}
		PackageEntry entry;
		entry.name.assign(index.data() + at, nameLength);
		at += nameLength;
		entry.offset = u32(p + at);
		entry.size = u32(p + at + 4);
		entry.crc32 = u32(p + at + 8);
		at += 12;
		if (!isSafeEntryName(entry.name)) {
			error = "package has an unsafe entry name";
			return false;
		}
		const std::uint64_t end = static_cast<std::uint64_t>(entry.offset) + entry.size;
		if (end > fileSize) {
			error = "package entry \"" + entry.name + "\" is out of bounds";
			return false;
		}
		// Entry data may not overlap the header or the index.
		const std::uint64_t indexEnd = static_cast<std::uint64_t>(indexOffset) + indexSize;
		if (entry.size && (entry.offset < kPackageHeaderSize || (entry.offset < indexEnd && end > indexOffset))) {
			error = "package entry \"" + entry.name + "\" overlaps the index";
			return false;
		}
		entries.push_back(std::move(entry));
	}
	if (at != index.size()) {
		error = "package index has trailing bytes";
		return false;
	}
	std::vector<const PackageEntry *> sorted;
	for (const PackageEntry &entry : entries) sorted.push_back(&entry);
	std::sort(sorted.begin(), sorted.end(), [](const PackageEntry *a, const PackageEntry *b) { return a->name < b->name; });
	for (std::size_t i = 1; i < sorted.size(); i++) {
		if (sorted[i]->name == sorted[i - 1]->name) {
			error = "package has duplicate entry \"" + sorted[i]->name + "\"";
			return false;
		}
	}
	std::sort(sorted.begin(), sorted.end(), [](const PackageEntry *a, const PackageEntry *b) { return a->offset < b->offset; });
	for (std::size_t i = 1; i < sorted.size(); i++) {
		const PackageEntry &prev = *sorted[i - 1];
		if (sorted[i]->size && prev.size && static_cast<std::uint64_t>(prev.offset) + prev.size > sorted[i]->offset) {
			error = "package entries overlap";
			return false;
		}
	}

	entries_ = std::move(entries);
	file_ = std::move(file);
	return true;
}

const PackageEntry *PackageReader::find(std::string_view name) const
{
	for (const PackageEntry &entry : entries_)
		if (entry.name == name) return &entry;
	return nullptr;
}

bool PackageReader::read(const PackageEntry &entry, std::string &out, std::string &error)
{
	if (!file_) {
		error = "package is not open";
		return false;
	}
	out.assign(entry.size, '\0');
	if (entry.size && !file_->readAt(entry.offset, out.data(), entry.size)) {
		error = "cannot read \"" + entry.name + "\"";
		return false;
	}
	if (crc32(out.data(), out.size()) != entry.crc32) {
		error = "\"" + entry.name + "\" is corrupt";
		return false;
	}
	return true;
}

bool PackageReader::read(std::string_view name, std::string &out, std::string &error)
{
	const PackageEntry *entry = find(name);
	if (!entry) {
		error = "package has no \"" + std::string(name) + "\"";
		return false;
	}
	return read(*entry, out, error);
}

bool PackageWriter::add(std::string_view name, std::string_view data)
{
	if (!isSafeEntryName(name) || entries_.size() >= kMaxPackageEntries) return false;
	for (const auto &entry : entries_)
		if (entry.first == name) return false;
	entries_.emplace_back(std::string(name), std::string(data));
	return true;
}

std::string PackageWriter::finish() const
{
	std::size_t indexSize = 0;
	for (const auto &entry : entries_) indexSize += 2 + entry.first.size() + 12;
	std::string index;
	std::uint32_t offset = static_cast<std::uint32_t>(kPackageHeaderSize + indexSize);
	for (const auto &entry : entries_) {
		put16(index, static_cast<std::uint16_t>(entry.first.size()));
		index += entry.first;
		put32(index, offset);
		put32(index, static_cast<std::uint32_t>(entry.second.size()));
		put32(index, crc32(entry.second.data(), entry.second.size()));
		offset += static_cast<std::uint32_t>(entry.second.size());
	}
	std::string out(kMagic, sizeof(kMagic));
	put16(out, kPackageFormat);
	put16(out, static_cast<std::uint16_t>(entries_.size()));
	put32(out, static_cast<std::uint32_t>(kPackageHeaderSize));
	put32(out, static_cast<std::uint32_t>(index.size()));
	put32(out, crc32(index.data(), index.size()));
	out.append(kPackageHeaderSize - out.size(), '\0');
	out += index;
	for (const auto &entry : entries_) out += entry.second;
	return out;
}

}  // namespace gea::geaos
