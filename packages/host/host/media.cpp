// SPDX-License-Identifier: Apache-2.0
#include "host/media.h"
#include "audio.h"
#include "host/pcm_stream.h"
#include "host/worker.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <stdexcept>
#include <sys/stat.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include "platform/file_cache.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/types.h>
#endif

namespace gea::host {

namespace {

struct PcmCursor {
  std::uint64_t next = 0;
  std::uint64_t dropped = 0;
  std::optional<std::uint64_t> end;
};

using PcmBuffer = std::deque<std::int16_t, pcm::QueueAllocator<std::int16_t>>;

struct TrackState {
  std::string id;
  std::string kind = "audio";
  std::string readyState = "live";
  bool enabled = true;
  PcmBuffer ringBuffer;
  bool capture = true;
  std::uint64_t bufferStart = 0;
  PcmCursor transportCursor;
  bool transportCursorActive = false;
  std::uint64_t nextReader = 1;
  std::unordered_map<std::uint64_t, PcmCursor> readers;
  std::shared_ptr<const media::VideoFrame> videoFrame;
  std::uint64_t videoSequence = 0;
};

void clearTrackPcm(TrackState &track) {
  track.bufferStart += track.ringBuffer.size();
  PcmBuffer().swap(track.ringBuffer);
  track.transportCursor.next = track.bufferStart;
  for (auto &[id, reader] : track.readers) reader.next = track.bufferStart;
}

void trimTrackPcm(TrackState &track) {
  if (!track.transportCursorActive && track.readers.empty()) return;
  auto consumed = track.transportCursorActive ? track.transportCursor.next : track.bufferStart + track.ringBuffer.size();
  for (const auto &[id, reader] : track.readers) consumed = std::min(consumed, reader.next);
  while (!track.ringBuffer.empty() && track.bufferStart < consumed) {
    track.ringBuffer.pop_front();
    ++track.bufferStart;
  }
}

std::size_t readTrackPcm(TrackState &track, PcmCursor &cursor, std::int16_t *samples, std::size_t count) {
  if (!samples || !count) return 0;
  if (cursor.next < track.bufferStart) {
    cursor.dropped += track.bufferStart - cursor.next;
    cursor.next = track.bufferStart;
  }
  const auto offset = static_cast<std::size_t>(cursor.next - track.bufferStart);
  if (cursor.end) {
    if (cursor.next >= *cursor.end) return 0;
    count = std::min(count, static_cast<std::size_t>(*cursor.end - cursor.next));
  }
  const auto copied = std::min(count, track.ringBuffer.size() - offset);
  for (std::size_t i = 0; i < copied; ++i) samples[i] = track.ringBuffer[offset + i];
  cursor.next += copied;
  trimTrackPcm(track);
  return copied;
}

struct StreamState {
  std::string id;
  std::vector<NativeMediaTrackHandle> tracks;
  NativeMediaTrackHandle ownedTrack = 0;
};

struct RecorderState {
  std::string id;
  NativeMediaStreamHandle stream = 0;
  NativeMediaTrackHandle track = 0;
  std::string state = "inactive";
  std::string path;
  std::string mimeType = "audio/wav";
  std::vector<std::int16_t> pcm;
  double sampleRate = gea::platform::audio::deviceSampleRate;
  double channels = 1.0;
  FILE *file = nullptr;
  std::size_t samplesWritten = 0;
  bool writeFailed = false;
  bool defaultPath = false;
};

std::unordered_map<NativeMediaStreamHandle, StreamState> &streamTable() {
  static std::unordered_map<NativeMediaStreamHandle, StreamState> table;
  return table;
}

std::unordered_map<NativeMediaTrackHandle, TrackState> &trackTable() {
  static std::unordered_map<NativeMediaTrackHandle, TrackState> table;
  return table;
}

std::unordered_map<NativeMediaRecorderHandle, RecorderState> &recorderTable() {
  static std::unordered_map<NativeMediaRecorderHandle, RecorderState> table;
  return table;
}

std::mutex &mediaMutex() {
  static std::mutex m;
  return m;
}

std::atomic<NativeMediaStreamHandle> &nextStreamHandle() {
  static std::atomic<NativeMediaStreamHandle> id{1};
  return id;
}

std::atomic<NativeMediaTrackHandle> &nextTrackHandle() {
  static std::atomic<NativeMediaTrackHandle> id{1};
  return id;
}

std::atomic<NativeMediaRecorderHandle> &nextRecorderHandle() {
  static std::atomic<NativeMediaRecorderHandle> id{1};
  return id;
}

std::string formatHandleId(const char *prefix, std::uint32_t handle) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%s-%u", prefix, static_cast<unsigned>(handle));
  return buf;
}

constexpr std::size_t kCaptureChunkSamples = 2048;
// Eight seconds at the capture driver's configured device rate. Storage grows
// only when a consumer falls behind, using PSRAM instead of I2S DMA memory.
constexpr std::size_t kMaxRingBufferSamples = 8 * gea::platform::audio::deviceSampleRate;
constexpr std::size_t kRecorderFlushSamples = kCaptureChunkSamples;
constexpr unsigned kMaxDefaultRecordingNames = 1000;
constexpr double kDefaultRecorderSampleRate = gea::platform::audio::deviceSampleRate;

#ifdef ESP_PLATFORM
constexpr const char *kMediaLogTag = "gea_media";
#endif

void makeParentDirs(const std::string &path) {
  std::size_t pos = 1;
  while ((pos = path.find('/', pos)) != std::string::npos) {
    const std::string dir = path.substr(0, pos);
    if (!dir.empty()) {
#if defined(_WIN32)
      _mkdir(dir.c_str());
#else
      mkdir(dir.c_str(), 0775);
#endif
    }
    ++pos;
  }
}

void writeLe16(FILE *file, std::uint16_t value) {
  std::uint8_t bytes[2] = {
      static_cast<std::uint8_t>(value & 0xffu),
      static_cast<std::uint8_t>((value >> 8) & 0xffu),
  };
  std::fwrite(bytes, 1, sizeof(bytes), file);
}

void writeLe32(FILE *file, std::uint32_t value) {
  std::uint8_t bytes[4] = {
      static_cast<std::uint8_t>(value & 0xffu),
      static_cast<std::uint8_t>((value >> 8) & 0xffu),
      static_cast<std::uint8_t>((value >> 16) & 0xffu),
      static_cast<std::uint8_t>((value >> 24) & 0xffu),
  };
  std::fwrite(bytes, 1, sizeof(bytes), file);
}

std::uint32_t wavDataBytesForSamples(std::size_t sampleCount) {
  const auto bytes = sampleCount * sizeof(std::int16_t);
  return static_cast<std::uint32_t>(bytes);
}

void writeWavHeader(FILE *file, std::uint32_t dataBytes, int sampleRate, int channels) {
  const std::uint32_t riffBytes = 36u + dataBytes;
  const std::uint16_t channelCount = static_cast<std::uint16_t>(channels <= 0 ? 1 : channels);
  const std::uint32_t rate = static_cast<std::uint32_t>(sampleRate <= 0 ? kDefaultRecorderSampleRate : sampleRate);
  const std::uint16_t bitsPerSample = 16;
  const std::uint16_t blockAlign = static_cast<std::uint16_t>(channelCount * (bitsPerSample / 8));
  const std::uint32_t byteRate = rate * blockAlign;

  std::fwrite("RIFF", 1, 4, file);
  writeLe32(file, riffBytes);
  std::fwrite("WAVE", 1, 4, file);
  std::fwrite("fmt ", 1, 4, file);
  writeLe32(file, 16);
  writeLe16(file, 1);
  writeLe16(file, channelCount);
  writeLe32(file, rate);
  writeLe32(file, byteRate);
  writeLe16(file, blockAlign);
  writeLe16(file, bitsPerSample);
  std::fwrite("data", 1, 4, file);
  writeLe32(file, dataBytes);
}

bool patchWavHeader(FILE *file, std::uint32_t dataBytes) {
  if (!file) return false;
  if (std::fflush(file) != 0) return false;
  const std::uint32_t riffBytes = 36u + dataBytes;
  if (std::fseek(file, 4, SEEK_SET) != 0) return false;
  writeLe32(file, riffBytes);
  if (std::fseek(file, 40, SEEK_SET) != 0) return false;
  writeLe32(file, dataBytes);
  return std::fflush(file) == 0;
}

double writeWavFile(const std::string &path, const std::vector<std::int16_t> &samples, int sampleRate, int channels) {
  if (path.empty()) return 0.0;
  gea::platform::storage::ensureMounted();
  makeParentDirs(path);

  FILE *file = std::fopen(path.c_str(), "wb");
  if (!file) {
#ifdef ESP_PLATFORM
    ESP_LOGE(kMediaLogTag, "failed to open WAV for write path=%s errno=%d", path.c_str(), errno);
#endif
    return 0.0;
  }

  const std::uint32_t dataBytes = wavDataBytesForSamples(samples.size());
  writeWavHeader(file, dataBytes, sampleRate, channels);
  std::size_t writtenSamples = 0;
  while (writtenSamples < samples.size()) {
    const std::size_t remaining = samples.size() - writtenSamples;
    const std::size_t chunkSamples = std::min<std::size_t>(remaining, 2048);
    const std::size_t written = std::fwrite(samples.data() + writtenSamples, sizeof(std::int16_t), chunkSamples, file);
    writtenSamples += written;
    if (written != chunkSamples) break;
  }
  const std::uint32_t actualDataBytes = static_cast<std::uint32_t>(writtenSamples * sizeof(std::int16_t));
  if (actualDataBytes != dataBytes) {
    patchWavHeader(file, actualDataBytes);
#ifdef ESP_PLATFORM
    ESP_LOGW(kMediaLogTag,
             "partial WAV write path=%s samples=%u/%u errno=%d",
             path.c_str(),
             static_cast<unsigned>(writtenSamples),
             static_cast<unsigned>(samples.size()),
             errno);
#endif
  }
  const bool ok = std::fclose(file) == 0;
  if (!ok) {
#ifdef ESP_PLATFORM
    ESP_LOGE(kMediaLogTag, "failed to close WAV path=%s errno=%d", path.c_str(), errno);
#endif
    return 0.0;
  }
  return static_cast<double>(44u + actualDataBytes);
}

bool recordingPathAssigned(const std::string &path) {
  for (const auto &entry : recorderTable()) {
    if (entry.second.path == path) return true;
  }
  return false;
}

// Recorder handles restart at 1 on every boot, so a name derived from the handle alone would overwrite
// a recording left from before the reboot. Picks the first name no recorder holds and that is definitely
// absent from the card; empty when there is none, so recording fails instead of overwriting.
// Call with mediaMutex() held, so two recorders cannot pick the same name.
std::string nextDefaultRecordingPath(NativeMediaRecorderHandle handle) {
  gea::platform::storage::ensureMounted();
  char buf[96];
  const auto first = static_cast<unsigned>(handle);
  for (unsigned number = first; number < first + kMaxDefaultRecordingNames; ++number) {
    std::snprintf(buf, sizeof(buf), "/sdcard/recordings/recording_%03u.wav", number);
    if (recordingPathAssigned(buf)) continue;

    struct stat info;
    if (stat(buf, &info) != 0 && errno == ENOENT) return buf;
  }
  return {};
}

// A default name is claimed by creating the file ("x" fails if it exists), so it never replaces a file that
// appeared after the name was picked, or this recorder's previous recording; it moves on to the next name.
std::FILE *openRecorderOutput(RecorderState &recorder, NativeMediaRecorderHandle handle) {
  if (!recorder.defaultPath) return std::fopen(recorder.path.c_str(), "wb");

  for (;;) {
    std::FILE *file = std::fopen(recorder.path.c_str(), "wbx");
    if (file || errno != EEXIST) return file;

    recorder.path = nextDefaultRecordingPath(handle);
    if (recorder.path.empty()) return nullptr;
  }
}

bool openRecorderFile(RecorderState &recorder, NativeMediaRecorderHandle handle) {
  if (recorder.path.empty()) return false;
  gea::platform::storage::ensureMounted();
  makeParentDirs(recorder.path);
  recorder.file = openRecorderOutput(recorder, handle);
  recorder.samplesWritten = 0;
  recorder.writeFailed = false;
  if (!recorder.file) {
    recorder.writeFailed = true;
#ifdef ESP_PLATFORM
    ESP_LOGE(kMediaLogTag, "failed to open recorder WAV path=%s errno=%d", recorder.path.c_str(), errno);
#endif
    return false;
  }
  writeWavHeader(recorder.file, 0, static_cast<int>(recorder.sampleRate), static_cast<int>(recorder.channels));
  return true;
}

void closeRecorderFile(RecorderState &recorder) {
  if (!recorder.file) return;
  std::fclose(recorder.file);
  recorder.file = nullptr;
}

bool flushRecorderPcm(RecorderState &recorder) {
  if (!recorder.file || recorder.pcm.empty() || recorder.writeFailed) return !recorder.writeFailed;
  std::size_t writtenSamples = 0;
  while (writtenSamples < recorder.pcm.size()) {
    const std::size_t remaining = recorder.pcm.size() - writtenSamples;
    const std::size_t chunkSamples = std::min<std::size_t>(remaining, 2048);
    const std::size_t written =
        std::fwrite(recorder.pcm.data() + writtenSamples, sizeof(std::int16_t), chunkSamples, recorder.file);
    recorder.samplesWritten += written;
    writtenSamples += written;
    if (written != chunkSamples) {
      recorder.writeFailed = true;
#ifdef ESP_PLATFORM
      ESP_LOGW(kMediaLogTag,
               "recorder WAV write stopped path=%s samples=%u errno=%d",
               recorder.path.c_str(),
               static_cast<unsigned>(recorder.samplesWritten),
               errno);
#endif
      recorder.pcm.clear();
      return false;
    }
  }
  recorder.pcm.clear();
  return true;
}

void appendRecorderPcm(RecorderState &recorder, const std::int16_t *samples, std::size_t count) {
  if (!recorder.file || recorder.writeFailed || !samples || count == 0) return;
  recorder.pcm.insert(recorder.pcm.end(), samples, samples + count);
  if (recorder.pcm.size() >= kRecorderFlushSamples) flushRecorderPcm(recorder);
}

double finalizeRecorderFile(RecorderState &recorder) {
  flushRecorderPcm(recorder);
  std::vector<std::int16_t>().swap(recorder.pcm);  // the entry outlives the recording; free its staging
  if (!recorder.file) return 0.0;
  const auto dataBytes = wavDataBytesForSamples(recorder.samplesWritten);
  const bool patched = patchWavHeader(recorder.file, dataBytes);
  const bool closed = std::fclose(recorder.file) == 0;
  recorder.file = nullptr;
  if (!patched || !closed) {
    recorder.writeFailed = true;
#ifdef ESP_PLATFORM
    ESP_LOGE(kMediaLogTag,
             "failed to finalize recorder WAV path=%s patched=%d closed=%d errno=%d",
             recorder.path.c_str(),
             patched ? 1 : 0,
             closed ? 1 : 0,
             errno);
#endif
    return 0.0;
  }
  return static_cast<double>(44u + dataBytes);
}

std::vector<std::uint8_t> readFileBytes(const std::string &path) {
  if (path.empty()) return {};
  gea::platform::storage::ensureMounted();
  FILE *file = std::fopen(path.c_str(), "rb");
  if (!file) return {};
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  if (size <= 0) {
    std::fclose(file);
    return {};
  }
  std::fseek(file, 0, SEEK_SET);
  std::vector<std::uint8_t> out(static_cast<std::size_t>(size));
  const std::size_t read = std::fread(out.data(), 1, out.size(), file);
  std::fclose(file);
  out.resize(read);
  return out;
}

}  // namespace

namespace media {

#ifdef ESP_PLATFORM
void platform_attach_track(NativeMediaTrackHandle handle);
void platform_detach_track(NativeMediaTrackHandle handle);
#else
__attribute__((weak)) void platform_attach_track(NativeMediaTrackHandle) {}
__attribute__((weak)) void platform_detach_track(NativeMediaTrackHandle) {}
#endif

NativeMediaStreamHandle create_audio_stream(bool capture) {
  auto streamHandle = nextStreamHandle()++;
  auto trackHandle = nextTrackHandle()++;
  {
    std::lock_guard<std::mutex> lock(mediaMutex());
    trackTable()[trackHandle] = TrackState{
        formatHandleId("track", trackHandle), "audio", "live", true, {}, capture};
    streamTable()[streamHandle] = StreamState{
        formatHandleId("stream", streamHandle), {trackHandle}, trackHandle};
  }
  if (capture) platform_attach_track(trackHandle);
  return streamHandle;
}
NativeMediaStreamHandle create_stream() { return create_audio_stream(true); }
NativeMediaStreamHandle create_remote_stream() { return create_audio_stream(false); }

NativeMediaStreamHandle create_remote_video_stream() {
  const auto streamHandle = nextStreamHandle()++;
  const auto trackHandle = nextTrackHandle()++;
  std::lock_guard<std::mutex> lock(mediaMutex());
  trackTable()[trackHandle] = TrackState{
      formatHandleId("track", trackHandle), "video", "live", true, {}, false};
  streamTable()[streamHandle] = StreamState{
      formatHandleId("stream", streamHandle), {trackHandle}, trackHandle};
  return streamHandle;
}

bool publish_video_frame(NativeMediaTrackHandle handle, std::shared_ptr<VideoFrame> frame) {
  if (!frame || !frame->width || !frame->height || frame->width > 1920 || frame->height > 1080 ||
      frame->rgb565.size() != std::size_t(frame->width) * frame->height) return false;
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = trackTable().find(handle);
  if (it == trackTable().end() || it->second.kind != "video" ||
      !it->second.enabled || it->second.readyState != "live") return false;
  frame->sequence = ++it->second.videoSequence;
  it->second.videoFrame = std::move(frame);
  return true;
}

std::shared_ptr<const VideoFrame> latest_video_frame(NativeMediaTrackHandle handle) {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = trackTable().find(handle);
  return it == trackTable().end() ? nullptr : it->second.videoFrame;
}

NativeMediaStreamHandle create_stream_from_tracks(const std::vector<MediaStreamTrack> &tracks) {
  std::lock_guard<std::mutex> lock(mediaMutex());
  std::vector<NativeMediaTrackHandle> members;
  for (const auto &track : tracks) {
    if (trackTable().find(track.nativeHandle) == trackTable().end())
      throw std::invalid_argument("MediaStream requires valid media tracks");
    if (std::find(members.begin(), members.end(), track.nativeHandle) == members.end())
      members.push_back(track.nativeHandle);
  }
  const auto handle = nextStreamHandle()++;
  streamTable()[handle] = StreamState{formatHandleId("stream", handle), std::move(members), 0};
  return handle;
}

NativeMediaTrackHandle stream_audio_track(NativeMediaStreamHandle handle) {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = streamTable().find(handle);
  if (it == streamTable().end()) return 0;
  for (auto handle : it->second.tracks) {
    const auto track = trackTable().find(handle);
    if (track != trackTable().end() && track->second.kind == "audio") return handle;
  }
  return 0;
}

void destroy_stream(NativeMediaStreamHandle handle) {
  NativeMediaTrackHandle trackHandle = 0;
  bool capture = false;
  {
    std::lock_guard<std::mutex> lock(mediaMutex());
    auto it = streamTable().find(handle);
    if (it == streamTable().end()) return;
    trackHandle = it->second.ownedTrack;
    auto track = trackTable().find(trackHandle);
    capture = track != trackTable().end() && track->second.capture && track->second.enabled && track->second.readyState != "ended";
    for (auto &entry : recorderTable()) {
      if (entry.second.track == trackHandle && entry.second.state == "recording") {
        finalizeRecorderFile(entry.second);
        entry.second.state = "inactive";
      }
    }
    // Destroying a container must not destroy tracks shared with another
    // stream. The transport/capture owner ends its source, retaining ended
    // track identity while another container still names it.
    if (track != trackTable().end()) {
      track->second.readyState = "ended";
      clearTrackPcm(track->second);
      track->second.videoFrame.reset();
      const bool shared = std::any_of(streamTable().begin(), streamTable().end(), [&](const auto &entry) {
        return entry.first != handle && std::find(entry.second.tracks.begin(), entry.second.tracks.end(), trackHandle) != entry.second.tracks.end();
      });
      if (!shared) trackTable().erase(track);
    }
    streamTable().erase(it);
  }
  if (trackHandle && capture) platform_detach_track(trackHandle);
}

void track_inject_pcm(NativeMediaTrackHandle handle, const std::int16_t *samples, std::size_t count) {
  if (!samples || !count) return;
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = trackTable().find(handle);
  if (!samples || it == trackTable().end() || it->second.kind != "audio" || !it->second.enabled || it->second.readyState == "ended") return;
  auto &buffer = it->second.ringBuffer;
  for (std::size_t i = 0; i < count; ++i) {
    if (buffer.size() >= kMaxRingBufferSamples) {
      buffer.pop_front();
      ++it->second.bufferStart;
    }
    buffer.push_back(samples[i]);
  }
  for (auto &entry : recorderTable()) {
    auto &recorder = entry.second;
    if (recorder.track == handle && recorder.state == "recording") {
      appendRecorderPcm(recorder, samples, count);
    }
  }
}

std::size_t track_read_pcm(NativeMediaTrackHandle handle, std::int16_t *samples, std::size_t max_samples) {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = trackTable().find(handle);
  if (it == trackTable().end()) return 0;
  if (!it->second.transportCursorActive) {
    it->second.transportCursorActive = true;
    it->second.transportCursor.next = it->second.bufferStart;
  }
  return readTrackPcm(it->second, it->second.transportCursor, samples, max_samples);
}

TrackPcmReader::TrackPcmReader(NativeMediaTrackHandle track, bool includeBuffered) {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = trackTable().find(track);
  if (it == trackTable().end() || it->second.readyState == "ended") return;
  track_ = track;
  reader_ = it->second.nextReader++;
  it->second.readers.emplace(reader_, PcmCursor{it->second.bufferStart + (includeBuffered ? 0 : it->second.ringBuffer.size()), 0});
  trimTrackPcm(it->second);
}

TrackPcmReader::~TrackPcmReader() { release(); }

TrackPcmReader::TrackPcmReader(TrackPcmReader &&other) noexcept
    : track_(std::exchange(other.track_, 0)), reader_(std::exchange(other.reader_, 0)) {}

TrackPcmReader &TrackPcmReader::operator=(TrackPcmReader &&other) noexcept {
  if (this != &other) {
    release();
    track_ = std::exchange(other.track_, 0);
    reader_ = std::exchange(other.reader_, 0);
  }
  return *this;
}

void TrackPcmReader::release() {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = trackTable().find(track_);
  if (it != trackTable().end()) {
    it->second.readers.erase(reader_);
    trimTrackPcm(it->second);
  }
  track_ = 0;
  reader_ = 0;
}

std::size_t TrackPcmReader::read(std::int16_t *samples, std::size_t count) {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = trackTable().find(track_);
  if (it == trackTable().end()) return 0;
  auto cursor = it->second.readers.find(reader_);
  if (cursor == it->second.readers.end()) return 0;
  return readTrackPcm(it->second, cursor->second, samples, count);
}

std::uint64_t TrackPcmReader::droppedSamples() const {
  std::lock_guard<std::mutex> lock(mediaMutex());
  const auto it = trackTable().find(track_);
  if (it == trackTable().end()) return 0;
  const auto cursor = it->second.readers.find(reader_);
  if (cursor == it->second.readers.end()) return 0;
  return cursor->second.dropped + (cursor->second.next < it->second.bufferStart ? it->second.bufferStart - cursor->second.next : 0);
}

std::size_t TrackPcmReader::pendingSamples() const {
  std::lock_guard<std::mutex> lock(mediaMutex());
  const auto it = trackTable().find(track_);
  if (it == trackTable().end()) return 0;
  const auto cursor = it->second.readers.find(reader_);
  if (cursor == it->second.readers.end()) return 0;
  auto end = it->second.bufferStart + it->second.ringBuffer.size();
  if (cursor->second.end) end = std::min(end, *cursor->second.end);
  const auto next = std::max(cursor->second.next, it->second.bufferStart);
  return end > next ? end - next : 0;
}

void TrackPcmReader::discardBuffered() {
  std::lock_guard<std::mutex> lock(mediaMutex());
  const auto it = trackTable().find(track_);
  if (it == trackTable().end()) return;
  const auto cursor = it->second.readers.find(reader_);
  if (cursor == it->second.readers.end()) return;
  cursor->second.next = it->second.bufferStart + it->second.ringBuffer.size();
  cursor->second.end.reset();
  trimTrackPcm(it->second);
}

void TrackPcmReader::beginDrain() {
  std::lock_guard<std::mutex> lock(mediaMutex());
  const auto it = trackTable().find(track_);
  if (it == trackTable().end()) return;
  const auto cursor = it->second.readers.find(reader_);
  if (cursor == it->second.readers.end() || cursor->second.end) return;
  // Snapshot once: an RTP track keeps delivering silence between replies.
  cursor->second.end = it->second.bufferStart + it->second.ringBuffer.size();
}

NativeMediaStreamHandle get_user_media_audio() {
  const auto handle = create_stream();
  const auto owner = workers::Context::current();
  if (!owner->isMain()) {
    try {
      owner->addCleanup([handle] { destroy_stream(handle); });
    } catch (...) {
      destroy_stream(handle);
      throw;
    }
  }
  return handle;
}

NativeMediaRecorderHandle create_recorder(NativeMediaStreamHandle stream, const std::string &path, const std::string &mimeType) {
  const auto recorderHandle = nextRecorderHandle()++;
  const auto trackHandle = stream_audio_track(stream);
  std::lock_guard<std::mutex> lock(mediaMutex());
  const std::string effectivePath = path.empty() ? nextDefaultRecordingPath(recorderHandle) : path;
  recorderTable()[recorderHandle] = RecorderState{
      formatHandleId("recorder", recorderHandle),
      stream,
      trackHandle,
      "inactive",
      effectivePath,
      mimeType.empty() ? std::string("audio/wav") : mimeType,
      {},
      kDefaultRecorderSampleRate,
      1.0,
  };
  recorderTable()[recorderHandle].defaultPath = path.empty();
  return recorderHandle;
}

std::string recorder_state(NativeMediaRecorderHandle handle) {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = recorderTable().find(handle);
  return it == recorderTable().end() ? std::string("inactive") : it->second.state;
}

std::string recorder_mime_type(NativeMediaRecorderHandle handle) {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = recorderTable().find(handle);
  return it == recorderTable().end() ? std::string("audio/wav") : it->second.mimeType;
}

void recorder_start(NativeMediaRecorderHandle handle) {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = recorderTable().find(handle);
  if (it == recorderTable().end()) return;
  closeRecorderFile(it->second);
  it->second.pcm.clear();
  it->second.samplesWritten = 0;
  it->second.writeFailed = false;
  if (openRecorderFile(it->second, handle)) {
    it->second.state = "recording";
  } else {
    it->second.state = "inactive";
  }
}

GeaAudioBlob recorder_stop(NativeMediaRecorderHandle handle) {
  GeaAudioBlob blob;
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = recorderTable().find(handle);
  if (it == recorderTable().end()) return {};
  it->second.state = "inactive";
  blob.path = it->second.path;
  blob.type = it->second.mimeType.empty() ? std::string("audio/wav") : it->second.mimeType;
  blob.size = finalizeRecorderFile(it->second);
  return blob;
}

}  // namespace media

std::vector<std::uint8_t> GeaAudioBlob::arrayBuffer() const {
  return readFileBytes(path);
}

std::string GeaAudioBlob::text() const {
  auto bytes = readFileBytes(path);
  return std::string(bytes.begin(), bytes.end());
}

MediaRecorderStateProperty::operator std::string() const {
  return media::recorder_state(nativeHandle);
}

MediaRecorder::MediaRecorder(MediaStream mediaStream)
    : MediaRecorder(mediaStream, std::string{}, std::string("audio/wav")) {}

MediaRecorder::MediaRecorder(MediaStream mediaStream, const std::string &path, const std::string &mimeType)
    : nativeHandle(media::create_recorder(mediaStream.nativeHandle, path, mimeType)),
      stream(mediaStream),
      state(nativeHandle) {}

std::string MediaRecorder::mimeType() const {
  return media::recorder_mime_type(nativeHandle);
}

void MediaRecorder::start(double timesliceMs) const {
  (void)timesliceMs;
  media::recorder_start(nativeHandle);
}

void MediaRecorder::stop() const {
  const auto blob = media::recorder_stop(nativeHandle);
  if (ondataavailable) ondataavailable(MediaRecorderDataEvent{blob});
  if (onstop) onstop();
}

std::string MediaStreamTrack::id() const {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = trackTable().find(nativeHandle);
  return it == trackTable().end() ? std::string{} : it->second.id;
}

std::string MediaStreamTrack::kind() const {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = trackTable().find(nativeHandle);
  return it == trackTable().end() ? std::string{} : it->second.kind;
}

std::string MediaStreamTrack::readyState() const {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = trackTable().find(nativeHandle);
  return it == trackTable().end() ? std::string("ended") : it->second.readyState;
}

bool MediaStreamTrack::enabled() const {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = trackTable().find(nativeHandle);
  return it == trackTable().end() ? false : it->second.enabled;
}

void MediaStreamTrack::setEnabled(bool value) const {
  bool capture = false;
  {
    std::lock_guard<std::mutex> lock(mediaMutex());
    auto it = trackTable().find(nativeHandle);
    if (it == trackTable().end() || it->second.readyState == "ended" || it->second.enabled == value) return;
    capture = it->second.capture;
    it->second.enabled = value;
    if (!value) {
      clearTrackPcm(it->second);
      it->second.videoFrame.reset();
    }
  }
  // Outside the media lock: the capture task itself injects PCM under that lock.
  if (capture) {
    if (value) media::platform_attach_track(nativeHandle);
    else media::platform_detach_track(nativeHandle);
  }
}

void MediaStreamTrack::stop() const {
  bool shouldDetach = false;
  {
    std::lock_guard<std::mutex> lock(mediaMutex());
    auto it = trackTable().find(nativeHandle);
    if (it != trackTable().end()) {
      shouldDetach = it->second.capture && it->second.enabled && it->second.readyState != "ended";
      it->second.readyState = "ended";
      it->second.enabled = false;
      // The entry stays for readyState lookups; the up-to-64 KB PCM ring buffer is freed now.
      clearTrackPcm(it->second);
      it->second.videoFrame.reset();
      for (auto &entry : recorderTable()) {
        if (entry.second.track == nativeHandle && entry.second.state == "recording") {
          entry.second.state = "inactive";
        }
      }
    }
  }
  if (shouldDetach) media::platform_detach_track(nativeHandle);
}

std::string MediaStream::id() const {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = streamTable().find(nativeHandle);
  return it == streamTable().end() ? std::string{} : it->second.id;
}

std::vector<MediaStreamTrack> MediaStream::getAudioTracks() const {
  std::lock_guard<std::mutex> lock(mediaMutex());
  auto it = streamTable().find(nativeHandle);
  std::vector<MediaStreamTrack> result;
  if (it != streamTable().end()) for (auto handle : it->second.tracks) {
    const auto track = trackTable().find(handle);
    if (track != trackTable().end() && track->second.kind == "audio") result.emplace_back(handle);
  }
  return result;
}

std::vector<MediaStreamTrack> MediaStream::getTracks() const {
  std::lock_guard<std::mutex> lock(mediaMutex());
  const auto it = streamTable().find(nativeHandle);
  std::vector<MediaStreamTrack> result;
  if (it != streamTable().end()) for (auto handle : it->second.tracks) result.emplace_back(handle);
  return result;
}

std::vector<MediaStreamTrack> MediaStream::getVideoTracks() const {
  std::lock_guard<std::mutex> lock(mediaMutex());
  const auto it = streamTable().find(nativeHandle);
  std::vector<MediaStreamTrack> result;
  if (it != streamTable().end()) for (auto handle : it->second.tracks) {
    const auto track = trackTable().find(handle);
    if (track != trackTable().end() && track->second.kind == "video") result.emplace_back(handle);
  }
  return result;
}

MediaStreamTrack MediaStream::getTrackById(const std::string &id) const {
  std::lock_guard<std::mutex> lock(mediaMutex());
  const auto it = streamTable().find(nativeHandle);
  if (it != streamTable().end()) for (auto handle : it->second.tracks) {
    const auto track = trackTable().find(handle);
    if (track != trackTable().end() && track->second.id == id) return MediaStreamTrack(handle);
  }
  return {};
}

bool MediaStream::active() const {
  std::lock_guard<std::mutex> lock(mediaMutex());
  const auto it = streamTable().find(nativeHandle);
  if (it != streamTable().end()) for (auto handle : it->second.tracks) {
    const auto track = trackTable().find(handle);
    if (track != trackTable().end() && track->second.readyState == "live") return true;
  }
  return false;
}

void MediaStream::addTrack(MediaStreamTrack track) const {
  std::lock_guard<std::mutex> lock(mediaMutex());
  const auto it = streamTable().find(nativeHandle);
  if (it == streamTable().end() || trackTable().find(track.nativeHandle) == trackTable().end())
    throw std::invalid_argument("addTrack requires a valid stream and track");
  auto &members = it->second.tracks;
  if (std::find(members.begin(), members.end(), track.nativeHandle) == members.end()) members.push_back(track.nativeHandle);
}

void MediaStream::removeTrack(MediaStreamTrack track) const {
  std::lock_guard<std::mutex> lock(mediaMutex());
  const auto it = streamTable().find(nativeHandle);
  if (it == streamTable().end()) throw std::invalid_argument("removeTrack requires a valid stream");
  auto &members = it->second.tracks;
  members.erase(std::remove(members.begin(), members.end(), track.nativeHandle), members.end());
}

}  // namespace gea::host
