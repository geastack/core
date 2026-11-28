// SPDX-License-Identifier: Apache-2.0
// The filesystem the registry reads on every target that has one: POSIX on
// macOS, Linux, Android and the ESP-IDF VFS; Win32 on Windows.
#include "geaos.h"

#include <cstdio>
#include <sys/stat.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#if !defined(ESP_PLATFORM)
#include <sys/statvfs.h>
#endif
#endif

#if defined(ESP_PLATFORM)
// ESP-IDF's VFS has no statvfs; the target that mounts a volume answers
// (FAT through esp_vfs_fat_info). Weak so a firmware without one links.
extern "C" __attribute__((weak)) bool gea_geaos_free_bytes(const char *path, unsigned long long *out);
#endif
#if defined(_WIN32) && !defined(S_ISREG)
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif

namespace gea::geaos {

bool Fs::readFile(const std::string &path, std::string &out, std::size_t maxBytes)
{
	std::unique_ptr<File> file = open(path);
	if (!file) return false;
	const std::uint64_t size = file->size();
	if (size > maxBytes) return false;
	out.assign(static_cast<std::size_t>(size), '\0');
	return size == 0 || file->readAt(0, out.data(), out.size());
}

bool Fs::removeTree(const std::string &path)
{
	std::vector<DirEntry> entries;
	if (listDir(path, entries)) {
		for (const DirEntry &entry : entries) removeTree(path + "/" + entry.name);
	}
	if (!exists(path)) return true;
	return remove(path) || !exists(path);
}

namespace {

class PosixFile final : public File {
public:
	PosixFile(std::FILE *file, std::uint64_t size) : file_(file), size_(size) {}
	~PosixFile() override { std::fclose(file_); }
	std::uint64_t size() const override { return size_; }
	bool readAt(std::uint64_t offset, void *buffer, std::size_t length) override
	{
		if (offset + length > size_) return false;
#if defined(_WIN32)
		if (_fseeki64(file_, static_cast<long long>(offset), SEEK_SET) != 0) return false;
#else
		if (fseeko(file_, static_cast<off_t>(offset), SEEK_SET) != 0) return false;
#endif
		return std::fread(buffer, 1, length, file_) == length;
	}

private:
	std::FILE *file_;
	std::uint64_t size_;
};

class PosixWritableFile final : public WritableFile {
public:
	explicit PosixWritableFile(std::FILE *file) : file_(file) {}
	~PosixWritableFile() override
	{
		if (file_) std::fclose(file_);
	}
	bool write(const void *data, std::size_t length) override
	{
		if (!file_ || failed_) return false;
		if (length && std::fwrite(data, 1, length, file_) != length) failed_ = true;
		return !failed_;
	}
	bool finish() override
	{
		if (!file_) return false;
		const bool flushed = std::fflush(file_) == 0;
#if !defined(_WIN32)
		// The data is on the card, not in a cache a pulled card loses, before
		// a rename makes it the app.
		const bool synced = fsync(fileno(file_)) == 0 || !flushed;
#else
		const bool synced = true;
#endif
		const bool closed = std::fclose(file_) == 0;
		file_ = nullptr;
		return !failed_ && flushed && synced && closed;
	}

private:
	std::FILE *file_;
	bool failed_ = false;
};

class PosixFs final : public Fs {
public:
	bool listDir(const std::string &path, std::vector<DirEntry> &out) override
	{
		out.clear();
#if defined(_WIN32)
		WIN32_FIND_DATAA data;
		HANDLE find = FindFirstFileA((path + "\\*").c_str(), &data);
		if (find == INVALID_HANDLE_VALUE) return false;
		do {
			const std::string name = data.cFileName;
			if (name.empty() || name == "." || name == "..") continue;
			DirEntry item;
			item.name = name;
			item.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
			item.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
			out.push_back(std::move(item));
		} while (FindNextFileA(find, &data));
		FindClose(find);
		return true;
#else
		DIR *dir = opendir(path.c_str());
		if (!dir) return false;
		while (dirent *entry = readdir(dir)) {
			const std::string name = entry->d_name;
			// Dot-names are listed: the installer's staging files are dot-named,
			// and removeTree must see a folder's hidden files to empty it. The
			// registry skips them.
			if (name.empty() || name == "." || name == "..") continue;
			struct stat info {};
			if (stat((path + "/" + name).c_str(), &info) != 0) continue;
			DirEntry item;
			item.name = name;
			item.directory = S_ISDIR(info.st_mode);
			item.size = static_cast<std::uint64_t>(info.st_size);
			out.push_back(std::move(item));
		}
		closedir(dir);
		return true;
#endif
	}

	bool exists(const std::string &path) override
	{
		struct stat info {};
		return stat(path.c_str(), &info) == 0;
	}

	std::unique_ptr<File> open(const std::string &path) override
	{
		struct stat info {};
		if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return nullptr;
#if defined(_WIN32)
		std::FILE *file = nullptr;
		if (fopen_s(&file, path.c_str(), "rb") != 0) file = nullptr;
#else
		std::FILE *file = std::fopen(path.c_str(), "rb");
#endif
		if (!file) return nullptr;
		return std::make_unique<PosixFile>(file, static_cast<std::uint64_t>(info.st_size));
	}

	std::unique_ptr<WritableFile> create(const std::string &path) override
	{
#if defined(_WIN32)
		std::FILE *file = nullptr;
		if (fopen_s(&file, path.c_str(), "wb") != 0) file = nullptr;
#else
		std::FILE *file = std::fopen(path.c_str(), "wb");
#endif
		if (!file) return nullptr;
		return std::make_unique<PosixWritableFile>(file);
	}

	bool makeDir(const std::string &path) override
	{
#if defined(_WIN32)
		if (_mkdir(path.c_str()) == 0) return true;
#else
		if (::mkdir(path.c_str(), 0775) == 0) return true;
#endif
		struct stat info {};
		return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
	}

	bool rename(const std::string &from, const std::string &to) override { return std::rename(from.c_str(), to.c_str()) == 0; }

	bool remove(const std::string &path) override
	{
		struct stat info {};
		if (stat(path.c_str(), &info) != 0) return false;
#if defined(_WIN32)
		if (S_ISDIR(info.st_mode)) return _rmdir(path.c_str()) == 0;
#else
		if (S_ISDIR(info.st_mode)) return ::rmdir(path.c_str()) == 0;
#endif
		return std::remove(path.c_str()) == 0;
	}

	std::optional<std::uint64_t> freeBytes(const std::string &path) override
	{
#if defined(ESP_PLATFORM)
		unsigned long long bytes = 0;
		if (gea_geaos_free_bytes && gea_geaos_free_bytes(path.c_str(), &bytes)) return bytes;
		return std::nullopt;
#elif defined(_WIN32)
		ULARGE_INTEGER available{};
		if (GetDiskFreeSpaceExA(path.c_str(), &available, nullptr, nullptr)) return available.QuadPart;
		return std::nullopt;
#else
		struct statvfs info {};
		if (statvfs(path.c_str(), &info) != 0) return std::nullopt;
		return static_cast<std::uint64_t>(info.f_bavail) * info.f_frsize;
#endif
	}
};

}  // namespace

Fs &posixFs()
{
	static PosixFs fs;
	return fs;
}

}  // namespace gea::geaos
