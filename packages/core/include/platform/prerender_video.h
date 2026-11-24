// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "canvas.h"
#include "display.h"

#include <bit>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace gea::platform::prerender {

class VideoFrames {
public:
	VideoFrames(int width, int height) : width_(width), height_(height)
	{
		std::fflush(stdout);
#ifdef _WIN32
		const int fd = _dup(_fileno(stdout));
		if (fd < 0) throw std::runtime_error("Could not open the frame stream");
		stream_.reset(_fdopen(fd, "wb"));
		if (!stream_) _close(fd);
		if (!stream_ || _setmode(fd, _O_BINARY) < 0 ||
		    !SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(fd)), HANDLE_FLAG_INHERIT, 0) || _dup2(_fileno(stderr), _fileno(stdout)) < 0)
#else
		const int fd = dup(fileno(stdout));
		if (fd < 0) throw std::runtime_error("Could not open the frame stream");
		stream_.reset(fdopen(fd, "wb"));
		if (!stream_) close(fd);
		if (!stream_ || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || dup2(fileno(stderr), fileno(stdout)) < 0)
#endif
			throw std::runtime_error("Could not redirect app logs away from the frame stream");
	}

	void write()
	{
		const auto *canvas = display::Display::canvas();
		const auto *pixels = canvas->pixels();
		const auto stride = canvas->strideBytes() / sizeof(*pixels);
		for (int y = 0; y < height_; ++y) {
			const auto *row = pixels + static_cast<std::size_t>(y) * stride;
			const auto *bytes = reinterpret_cast<const std::uint8_t *>(row);
			if constexpr (GEA_EMBEDDED_PIXEL_FORMAT != GEA_PIXEL_RGBA8888 || std::endian::native != std::endian::little) {
				converted_.resize(static_cast<std::size_t>(width_) * 4);
				auto *out = converted_.data();
				for (int x = 0; x < width_; ++x) {
					int r, g, b, a;
					framework::graphics::pixel::unpackNative8(row[x], &r, &g, &b, &a);
					*out++ = static_cast<std::uint8_t>(r);
					*out++ = static_cast<std::uint8_t>(g);
					*out++ = static_cast<std::uint8_t>(b);
					*out++ = static_cast<std::uint8_t>(a);
				}
				bytes = converted_.data();
			}
			if (std::fwrite(bytes, 4, width_, stream_.get()) != static_cast<std::size_t>(width_))
				throw std::runtime_error("Could not write a video frame");
		}
	}

	void finish()
	{
		if (std::fclose(stream_.release()) != 0)
			throw std::runtime_error("Could not finish the frame stream");
	}

private:
	int width_;
	int height_;
	struct CloseStream {
		void operator()(FILE *stream) const { std::fclose(stream); }
	};
	std::unique_ptr<FILE, CloseStream> stream_;
	std::vector<std::uint8_t> converted_;
};

using VideoTick = void (*)(int timestampMs);
inline VideoTick videoTick = nullptr;
inline int videoFps = 0;

class VideoClock {
public:
	explicit VideoClock(int fps) { videoFps = fps; }
	~VideoClock() { videoTick = nullptr; videoFps = 0; }
};

template <typename Frames, typename Paint>
void streamVideoFrames(Frames &stream, int fps, int startMs, int endMs, Paint paint)
{
	// Warm up on the app timeline. Sample the requested half-open range on
	// its own cadence so the first encoded frame is always exactly startMs.
	for (std::int64_t index = 0;; ++index) {
		const auto time = (index * 1000 + fps / 2) / fps;
		if (time >= startMs) break;
		if (videoTick) videoTick(static_cast<int>(time));
		paint(static_cast<int>(time));
	}
	for (std::int64_t index = 0; index * 1000 < static_cast<std::int64_t>(endMs - startMs) * fps; ++index) {
		const auto time = startMs + (index * 1000 + fps / 2) / fps;
		const int timestamp = static_cast<int>(time < endMs ? time : endMs - 1);
		if (videoTick) videoTick(timestamp);
		paint(timestamp);
		stream.write();
	}
	stream.finish();
}

} // namespace gea::platform::prerender
