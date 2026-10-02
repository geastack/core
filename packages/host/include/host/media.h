// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "host/video_pixels.h"

namespace gea::host {

using NativeMediaStreamHandle = std::uint32_t;
using NativeMediaTrackHandle = std::uint32_t;
using NativeMediaRecorderHandle = std::uint32_t;

class MediaStreamTrack {
 public:
  NativeMediaTrackHandle nativeHandle = 0;

  constexpr MediaStreamTrack() = default;
  explicit MediaStreamTrack(NativeMediaTrackHandle h) : nativeHandle(h) {}
  explicit MediaStreamTrack(double h) : nativeHandle(static_cast<NativeMediaTrackHandle>(h)) {}
  constexpr operator double() const { return static_cast<double>(nativeHandle); }

  template <typename Value>
  static MediaStreamTrack __gea_native_from_value(const Value &value) {
    if constexpr (requires { static_cast<double>(value); }) return MediaStreamTrack(static_cast<double>(value));
    else return MediaStreamTrack{};
  }

  std::string id() const;
  std::string kind() const;
  std::string readyState() const;
  bool enabled() const;
  void setEnabled(bool value) const;
  void stop() const;
};

class MediaStream {
 public:
  NativeMediaStreamHandle nativeHandle = 0;

  constexpr MediaStream() = default;
  explicit MediaStream(NativeMediaStreamHandle h) : nativeHandle(h) {}
  explicit MediaStream(double h) : nativeHandle(static_cast<NativeMediaStreamHandle>(h)) {}
  constexpr operator double() const { return static_cast<double>(nativeHandle); }

  template <typename Value>
  static MediaStream __gea_native_from_value(const Value &value) {
    if constexpr (requires { static_cast<double>(value); }) return MediaStream(static_cast<double>(value));
    else return MediaStream{};
  }

  std::string id() const;
  std::vector<MediaStreamTrack> getAudioTracks() const;
  std::vector<MediaStreamTrack> getVideoTracks() const;
  std::vector<MediaStreamTrack> getTracks() const;
  MediaStreamTrack getTrackById(const std::string &id) const;
  bool active() const;
  void addTrack(MediaStreamTrack track) const;
  void removeTrack(MediaStreamTrack track) const;
};

struct GeaAudioBlob {
  std::string path;
  std::string type = "audio/wav";
  double size = 0.0;

  template <typename Value>
  static GeaAudioBlob __gea_native_from_value(const Value &) { return {}; }

  std::vector<std::uint8_t> arrayBuffer() const;
  std::string text() const;
};

struct MediaRecorderDataEvent {
  GeaAudioBlob data;
};

namespace media {
template <typename Options>
std::string recorder_path_from_options(const Options &options);

template <typename Options>
std::string recorder_mime_from_options(const Options &options);
}  // namespace media

struct MediaRecorderStateProperty {
  NativeMediaRecorderHandle nativeHandle = 0;

  constexpr MediaRecorderStateProperty() = default;
  explicit constexpr MediaRecorderStateProperty(NativeMediaRecorderHandle handle) : nativeHandle(handle) {}

  operator std::string() const;
};

class MediaRecorder {
 public:
  NativeMediaRecorderHandle nativeHandle = 0;
  MediaStream stream;
  mutable MediaRecorderStateProperty state;
  std::function<void(MediaRecorderDataEvent)> ondataavailable = nullptr;
  std::function<void()> onstop = nullptr;

  MediaRecorder() = default;
  explicit MediaRecorder(double handle) : nativeHandle(static_cast<NativeMediaRecorderHandle>(handle)), state(nativeHandle) {}
  explicit MediaRecorder(MediaStream mediaStream);

  template <typename Options>
  MediaRecorder(MediaStream mediaStream, const Options &options)
      : MediaRecorder(mediaStream, media::recorder_path_from_options(options), media::recorder_mime_from_options(options)) {}

  MediaRecorder(MediaStream mediaStream, const std::string &path, const std::string &mimeType = "audio/wav");

  std::string mimeType() const;
  void start(double timesliceMs = 0.0) const;
  void stop() const;

  template <typename Value>
  static MediaRecorder __gea_native_from_value(const Value &value) {
    if constexpr (requires { static_cast<double>(value); }) return MediaRecorder(static_cast<double>(value));
    else return MediaRecorder{};
  }
};

namespace media {

NativeMediaStreamHandle create_stream();
// Stream membership is independent of the capture/receiver that owns a track.
NativeMediaStreamHandle create_stream_from_tracks(const std::vector<MediaStreamTrack> &tracks);
inline MediaStream create_media_stream() { return MediaStream(create_stream_from_tracks({})); }
inline MediaStream create_media_stream(const MediaStream &source) {
  return MediaStream(create_stream_from_tracks(source.getTracks()));
}
template <typename Tracks>
MediaStream create_media_stream(const Tracks &tracks) {
  if constexpr (requires { tracks.has_value(); *tracks; }) {
    return tracks.has_value() ? create_media_stream(*tracks) : create_media_stream();
  } else if constexpr (requires { tracks.size(); tracks.at(0); }) {
    std::vector<MediaStreamTrack> items;
    items.reserve(tracks.size());
    for (std::size_t i = 0; i < tracks.size(); ++i) items.push_back(tracks.at(i));
    return MediaStream(create_stream_from_tracks(items));
  } else if constexpr (requires { tracks.get(); *tracks; }) {
    return tracks.get() ? create_media_stream(*tracks) : create_media_stream();
  } else {
    static_assert(sizeof(Tracks) == 0, "MediaStream initializer must be a stream or a track sequence");
  }
}
// Received RTP audio must never attach to the microphone capture driver.
NativeMediaStreamHandle create_remote_stream();
NativeMediaStreamHandle create_remote_video_stream();
void destroy_stream(NativeMediaStreamHandle handle);
NativeMediaTrackHandle stream_audio_track(NativeMediaStreamHandle handle);

// Decoders publish ownership once. Every sink sees the newest immutable frame;
// a slow renderer cannot build a playback backlog or consume another sink's frame.
// RGB565 is logical host-endian unless panelEndian is set by a decoder for
// a matching embedded display. Other consumers normalize before conversion.
struct VideoFrame {
  std::uint32_t width = 0, height = 0;
  std::uint32_t timestampMs = 0;
  std::uint64_t sequence = 0;
  bool panelEndian = false;
  VideoPixels rgb565;
};
bool publish_video_frame(NativeMediaTrackHandle handle, std::shared_ptr<VideoFrame> frame);
std::shared_ptr<const VideoFrame> latest_video_frame(NativeMediaTrackHandle handle);

void track_inject_pcm(NativeMediaTrackHandle handle, const std::int16_t *samples, std::size_t count);
std::size_t track_read_pcm(NativeMediaTrackHandle handle, std::int16_t *samples, std::size_t max_samples);

// Each sink gets its own cursor into the bounded track buffer. A new sink
// starts at live audio, not at samples retained for an existing consumer.
class TrackPcmReader {
 public:
  explicit TrackPcmReader(NativeMediaTrackHandle track, bool includeBuffered = false);
  ~TrackPcmReader();
  TrackPcmReader(const TrackPcmReader &) = delete;
  TrackPcmReader &operator=(const TrackPcmReader &) = delete;
  TrackPcmReader(TrackPcmReader &&other) noexcept;
  TrackPcmReader &operator=(TrackPcmReader &&other) noexcept;
  std::size_t read(std::int16_t *samples, std::size_t count);
  std::uint64_t droppedSamples() const;
  std::size_t pendingSamples() const;
  void beginDrain();
  void discardBuffered();

 private:
  void release();
  NativeMediaTrackHandle track_ = 0;
  std::uint64_t reader_ = 0;
};

NativeMediaStreamHandle get_user_media_audio();
NativeMediaRecorderHandle create_recorder(NativeMediaStreamHandle stream, const std::string &path, const std::string &mimeType);
std::string recorder_state(NativeMediaRecorderHandle handle);
std::string recorder_mime_type(NativeMediaRecorderHandle handle);
void recorder_start(NativeMediaRecorderHandle handle);
GeaAudioBlob recorder_stop(NativeMediaRecorderHandle handle);

// Recorder options arrive as a plain struct, or from an app as a generated record that geatsc passes
// as gea::Optional<gea::Ref<Record>> with gea::Optional<std::string> fields. recorder_option unwraps
// each layer, so `new MediaRecorder(stream, { path })` from TS is honoured.
template <typename Value>
std::string option_string(const Value &value, const std::string &fallback) {
  if constexpr (requires { value.has_value(); *value; }) {
    return value.has_value() ? std::string(*value) : fallback;
  } else {
    return std::string(value);
  }
}

template <typename Options, typename Field>
std::string recorder_option(const Options &options, Field field, const std::string &fallback) {
  if constexpr (requires { options.has_value(); *options; }) {
    return options.has_value() ? recorder_option(*options, field, fallback) : fallback;
  } else if constexpr (requires { options.get(); *options; }) {
    return options.get() ? recorder_option(*options, field, fallback) : fallback;
  } else if constexpr (requires { field(options); }) {
    return option_string(field(options), fallback);
  } else {
    return fallback;
  }
}

template <typename Options>
std::string recorder_path_from_options(const Options &options) {
  return recorder_option(options, [](const auto &record) -> decltype(record.path) { return record.path; }, {});
}

template <typename Options>
std::string recorder_mime_from_options(const Options &options) {
  return recorder_option(
      options, [](const auto &record) -> decltype(record.mimeType) { return record.mimeType; }, "audio/wav");
}

// Overload accepting a MediaStreamConstraints-shaped record (from
// `navigator.mediaDevices.getUserMedia({...})`). The fields aren't acted on
// yet — there's no per-device picker on the embedded target — but accepting
// the arg lets the geatsc-emitted call site link. The single-arg form
// dispatches to the no-arg implementation.
template <typename Constraints>
NativeMediaStreamHandle get_user_media_audio(const Constraints &) { return get_user_media_audio(); }

}  // namespace media

}  // namespace gea::host
