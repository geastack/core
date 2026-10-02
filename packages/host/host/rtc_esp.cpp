// SPDX-License-Identifier: Apache-2.0
#ifdef ESP_PLATFORM

#include "host/rtc.h"
#include "host/media.h"
#include "host/rtc_opus.h"
#include "host/rtc_sdp.h"
#if __has_include("esp_h264_dec_sw.h")
#include "host/rtc_h264.h"
#define GEA_RTC_HAS_H264 1
#else
#define GEA_RTC_HAS_H264 0
#endif
#if __has_include("gea_vp8_peer.h") && __has_include("vpx/vpx_decoder.h")
#include "gea_vp8_peer.h"
#include "host/rtc_receive_sdp.h"
#include "host/rtc_vp8_decoder.h"
#define GEA_RTC_HAS_VP8 1
#if defined(GEA_EMBEDDED_VP8_BENCHMARK) && GEA_EMBEDDED_VP8_BENCHMARK
#include "host/rtc_vp8_capture.h"
#endif
#else
#define GEA_RTC_HAS_VP8 0
#endif
#define GEA_RTC_HAS_VIDEO (GEA_RTC_HAS_H264 || GEA_RTC_HAS_VP8)
#include "wifi.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <memory>
#include <optional>
#include <stdexcept>
#include <climits>
#include <string>
#include <unordered_map>
#include <vector>
#include <deque>

namespace gea::host::rtc {

namespace {

constexpr const char *kTag = "gea::host::rtc";
// RFC 7587 section 7: SDP/RTP always use opus/48000/2, even for 16 kHz
// mono PCM. The bitstream carries its actual bandwidth and channel count.
constexpr int kOpusRtpClockRate = 48000;
constexpr int kOpusRtpChannels = 2;
constexpr std::size_t kSendFrameSamples = OpusAudio::frameSamples;
// DTLS + SCTP call chains exceed 8 KiB on IDF 6. These are PSRAM stacks;
// retain headroom for Opus callbacks rather than consuming internal DMA RAM.
constexpr uint32_t kPeerStackBytes = 24 * 1024;
// The fixed-point SILK encoder uses nested variable-sized stack arrays;
// 16 KiB overflowed on the first real microphone frame at complexity 3.
constexpr uint32_t kSendStackBytes = 64 * 1024;
constexpr uint32_t kControlStackBytes = 32 * 1024;

// Read only this worker, without stack scanning or a global task census. The
// IDF runtime counter uses microseconds; preserve its configured width so
// subtraction remains correct across the 32-bit counter wrap.
configRUN_TIME_COUNTER_TYPE currentTaskCpuTicks() {
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
  TaskStatus_t info{};
  vTaskGetInfo(nullptr, &info, pdFALSE, eInvalid);
  return info.ulRunTimeCounter;
#else
  return 0;
#endif
}

struct EspPeerState {
  esp_peer_handle_t handle = nullptr;
  NativeRtcPeerHandle our_handle = 0;
  std::atomic<bool> closing{false};
  TaskHandle_t main_loop_task = nullptr;
  TaskHandle_t audio_send_task = nullptr;
  TaskHandle_t audio_receive_task = nullptr;

  // create_offer blocks on this until on_msg fires with SDP.
  SemaphoreHandle_t sdp_ready = nullptr;
  SemaphoreHandle_t transport_wake = nullptr;
  bool packet_wake = false;
  std::string pending_sdp;
  std::mutex sdp_mutex;

  std::atomic<NativeMediaTrackHandle> local_track{0};
  NativeMediaTrackHandle remote_track = 0;
  NativeMediaStreamHandle remote_stream = 0;

  std::atomic<bool> open{false};
  std::atomic<bool> connected{false};
  std::atomic<bool> data_connected{false};
  RTCConfiguration config;
  std::unique_ptr<OpusAudio> audio;
  bool received_audio = false;
  std::atomic<uint32_t> received_audio_frames{0};
  std::atomic<unsigned> received_opus_config{0};
  std::atomic<uint32_t> decode_time_us{0}, publish_time_us{0}, decode_count{0};
  struct EncodedAudio {
    std::array<uint8_t, 1275> bytes;
    uint32_t pts = 0;
    uint16_t size = 0;
  };
  std::deque<EncodedAudio, media::VideoAllocator<EncodedAudio>> received_audio_queue;
  std::mutex received_audio_mutex;
  std::atomic<uint32_t> received_audio_drops{0};
  int64_t last_audio_us = 0;
  uint32_t last_audio_pts = 0;
  std::string direction = "inactive", video_direction = "inactive";
  NativeMediaTrackHandle remote_video_track = 0;
  NativeMediaStreamHandle remote_video_stream = 0;
#if GEA_RTC_HAS_VIDEO
  bool vp8_video = false;
  bool native_daily_receive = false;
  unsigned input_video_width = 0, input_video_height = 0;
  std::atomic<bool> request_video_keyframe{false};
  using VideoBytes = std::vector<uint8_t, media::VideoAllocator<uint8_t>>;
  struct EncodedVideo { VideoBytes data; uint32_t pts = 0; };
  std::deque<EncodedVideo> video_frames;
  std::mutex video_mutex;
  bool need_video_keyframe = true;
  TaskHandle_t video_task = nullptr;
  SemaphoreHandle_t video_done = nullptr;
#endif
  std::vector<esp_peer_ice_server_cfg_t> servers;
  std::vector<std::pair<std::string, RTCDataChannelInit>> pending_channels;
  std::mutex channels_mutex;
  SemaphoreHandle_t main_done = nullptr, audio_done = nullptr, audio_receive_done = nullptr;
};

// SDP creation and teardown may wait for the transport. Serialize those
// operations on a PSRAM worker, never on the frame/input thread. Close queues
// behind any pending offer, so neither can free state the other still uses.
std::mutex controlMutex;
std::deque<std::function<void()>> controlJobs;
TaskHandle_t controlTask = nullptr;

void controlLoop(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    for (;;) {
      std::function<void()> job;
      {
        std::lock_guard<std::mutex> lock(controlMutex);
        if (controlJobs.empty()) break;
        job = std::move(controlJobs.front());
        controlJobs.pop_front();
      }
      try { job(); }
      catch (const std::exception& error) { ESP_LOGE(kTag, "RTC control operation failed: %s", error.what()); }
    }
  }
}

void ensureControlTask() {
  if (controlTask) return;
  if (xTaskCreatePinnedToCoreWithCaps(controlLoop, "gea_rtc_control", kControlStackBytes, nullptr, 2,
                                    &controlTask, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
    throw std::runtime_error("RTC control task allocation failed");
}

void queueControl(std::function<void()> job) {
  {
    std::lock_guard<std::mutex> lock(controlMutex);
    controlJobs.push_back(std::move(job));
  }
  xTaskNotifyGive(controlTask);
}

std::unordered_map<NativeRtcPeerHandle, EspPeerState *> &espPeerTable() {
  static std::unordered_map<NativeRtcPeerHandle, EspPeerState *> table;
  return table;
}

std::mutex &espPeerMutex() {
  static std::mutex m;
  return m;
}

EspPeerState *findPeer(NativeRtcPeerHandle handle) {
  std::lock_guard<std::mutex> lock(espPeerMutex());
  auto it = espPeerTable().find(handle);
  return it == espPeerTable().end() ? nullptr : it->second;
}

// Callbacks fire on esp_peer's main-loop task — we marshal events onto our
// JS-side frame-loop pump via the existing rtc::enqueue_* hooks.

int on_state_cb(esp_peer_state_t state, void *ctx) {
  ESP_LOGI(kTag, "transport state=%d", static_cast<int>(state));
  auto *peer_state = static_cast<EspPeerState *>(ctx);
  if (state == ESP_PEER_STATE_DATA_CHANNEL_CONNECTED) {
    peer_state->data_connected = true;
    return 0;
  }
  if (state == ESP_PEER_STATE_DATA_CHANNEL_DISCONNECTED) {
    peer_state->data_connected = false;
    return 0;
  }
  const char *name = "new";
  switch (state) {
    case ESP_PEER_STATE_CLOSED: name = "closed"; break;
    case ESP_PEER_STATE_DISCONNECTED: name = "disconnected"; break;
    case ESP_PEER_STATE_NEW_CONNECTION:
    case ESP_PEER_STATE_CANDIDATE_GATHERING:
    case ESP_PEER_STATE_PAIRING:
    case ESP_PEER_STATE_PAIRED:
    case ESP_PEER_STATE_CONNECTING:
      name = "connecting";
      break;
    case ESP_PEER_STATE_CONNECTED: name = "connected"; break;
    case ESP_PEER_STATE_CONNECT_FAILED: name = "failed"; break;
    default: return 0; // Data-channel/track events are not connection changes.
  }
  peer_state->connected = state == ESP_PEER_STATE_CONNECTED;
  enqueue_connection_state_change(peer_state->our_handle, std::string(name));
  enqueue_ice_connection_state_change(peer_state->our_handle,
      state == ESP_PEER_STATE_CONNECTED ? "connected" : state == ESP_PEER_STATE_CONNECT_FAILED ? "failed" :
      state == ESP_PEER_STATE_CLOSED ? "closed" : state == ESP_PEER_STATE_DISCONNECTED ? "disconnected" : "checking");
  return 0;
}

int on_msg_cb(esp_peer_msg_t *info, void *ctx) {
  auto *peer_state = static_cast<EspPeerState *>(ctx);
  if (!info || !info->data || info->size <= 0) return 0;
  std::string payload(reinterpret_cast<const char *>(info->data), static_cast<std::size_t>(info->size));
  switch (info->type) {
    case ESP_PEER_MSG_TYPE_SDP:
      ESP_LOGI(kTag, "local SDP ready (%d bytes)", info->size);
      // Candidate addresses are safe transport diagnostics; never log SDP
      // wholesale because it also contains ICE authentication credentials.
      for (std::size_t pos = payload.find("a=candidate:"); pos != std::string::npos;
           pos = payload.find("a=candidate:", pos + 1)) {
        const auto end = payload.find_first_of("\r\n", pos);
        ESP_LOGI(kTag, "local %s", payload.substr(pos, end - pos).c_str());
      }
      { std::lock_guard<std::mutex> lock(peer_state->sdp_mutex);
        peer_state->pending_sdp = opusPcmCapabilities(payload, OpusAudio::sampleRate, 1); }
      if (peer_state->sdp_ready) xSemaphoreGive(peer_state->sdp_ready);
      break;
    case ESP_PEER_MSG_TYPE_CANDIDATE:
      enqueue_ice_candidate(peer_state->our_handle, payload, std::string("0"), 0);
      break;
    default: break;
  }
  return 0;
}

int decode_audio_frame(esp_peer_audio_frame_t *frame, void *ctx) {
  auto *peer_state = static_cast<EspPeerState *>(ctx);
  if (!frame || !frame->data || frame->size <= 0) return 0;
  if (!peer_state->remote_track) {
    // Lazily create a remote track stream + track so JS can attach playback.
    peer_state->remote_stream = gea::host::media::create_remote_stream();
    peer_state->remote_track = gea::host::media::stream_audio_track(peer_state->remote_stream);
    enqueue_track(peer_state->our_handle, peer_state->remote_track);
  }
  const int64_t arrived = esp_timer_get_time();
  if (peer_state->last_audio_us && arrived - peer_state->last_audio_us > 60000)
    ESP_LOGI(kTag, "RTP audio gap_ms=%lld pts_delta=%u", static_cast<long long>((arrived - peer_state->last_audio_us) / 1000), unsigned(frame->pts - peer_state->last_audio_pts));
  peer_state->last_audio_us = arrived;
  peer_state->last_audio_pts = frame->pts;
  peer_state->received_opus_config.store(frame->data[0] >> 3, std::memory_order_relaxed);
  const int samples = peer_state->audio->decode(frame->data, frame->size);
  const int64_t decodeUs = esp_timer_get_time() - arrived;
  peer_state->decode_time_us += decodeUs;
  ++peer_state->decode_count;
  if (decodeUs > 15000) ESP_LOGI(kTag, "Opus decode_ms=%lld samples=%d", static_cast<long long>(decodeUs / 1000), samples);
  if (samples < 0) { ESP_LOGW(kTag, "Invalid Opus packet: %d", samples); return 0; }
  if (!peer_state->received_audio) {
    peer_state->received_audio = true;
    ESP_LOGI(kTag, "first decoded audio: samples=%d opus_config=%u stereo=%u worker stack free=%u/%u", samples,
      unsigned(frame->data[0] >> 3), unsigned((frame->data[0] >> 2) & 1),
      static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)), static_cast<unsigned>(kPeerStackBytes));
  }
  peer_state->received_audio_frames.fetch_add(1, std::memory_order_relaxed);
  gea::host::media::track_inject_pcm(peer_state->remote_track, peer_state->audio->decoded.data(), samples);
  peer_state->publish_time_us += esp_timer_get_time() - arrived - decodeUs;
  return 0;
}

int on_audio_data_cb(esp_peer_audio_frame_t* frame, void* ctx) {
  auto* state = static_cast<EspPeerState*>(ctx);
  if (!state->open || state->closing || !frame || !frame->data || frame->size <= 0 || frame->size > 1275)
    return -1;
  // The transport must keep draining authenticated RTP while Opus works.
  // Queue capacity absorbs bursts; the decoder starts immediately (no prefill).
  try {
    std::lock_guard lock(state->received_audio_mutex);
    if (state->received_audio_queue.size() >= 32) {
      ++state->received_audio_drops;
      return -1;
    }
    auto& packet = state->received_audio_queue.emplace_back();
    packet.size = frame->size;
    packet.pts = frame->pts;
    std::memcpy(packet.bytes.data(), frame->data, frame->size);
  } catch (const std::bad_alloc&) {
    ++state->received_audio_drops;
    return -1;
  }
  if (state->audio_receive_task) xTaskNotifyGive(state->audio_receive_task);
  return 0;
}

void audio_receive_trampoline(void* opaque) {
  auto* state = static_cast<EspPeerState*>(opaque);
  auto cpuStart = currentTaskCpuTicks();
  auto measuredAt = esp_timer_get_time();
  unsigned packets = 0;
  while (state->open && !state->closing) {
    EspPeerState::EncodedAudio packet;
    {
      std::lock_guard lock(state->received_audio_mutex);
      if (!state->received_audio_queue.empty()) {
        packet = std::move(state->received_audio_queue.front());
        state->received_audio_queue.pop_front();
      }
    }
    if (!packet.size) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
      continue;
    }
    esp_peer_audio_frame_t frame{};
    frame.data = packet.bytes.data();
    frame.size = packet.size;
    frame.pts = packet.pts;
    decode_audio_frame(&frame, state);
    ++packets;
    const auto now = esp_timer_get_time();
    if (now - measuredAt >= 2000000) {
      const auto cpuNow = currentTaskCpuTicks();
      ESP_LOGI(kTag, "audio worker peer=%u cpu_us=%llu span_us=%lld packets=%u",
          unsigned(state->our_handle),
          (unsigned long long)configRUN_TIME_COUNTER_TYPE(cpuNow - cpuStart),
          (long long)(now - measuredAt), packets);
      cpuStart = cpuNow; measuredAt = now; packets = 0;
    }
  }
  xSemaphoreGive(state->audio_receive_done);
  for (;;) vTaskSuspend(nullptr);
}

#if GEA_RTC_HAS_VIDEO
int queue_video_data(esp_peer_video_frame_t* frame, void* ctx,
                     EspPeerState::VideoBytes* owned = nullptr) {
  auto* state = static_cast<EspPeerState*>(ctx);
  if (!frame || !frame->data || frame->size <= 0 || frame->size > 512 * 1024 || !state->open) return 0;
  bool keyframe = false, headers = false;
  const size_t length = static_cast<size_t>(frame->size);
  if (state->vp8_video) {
    keyframe = !(frame->data[0] & 1);
    // VP8 keyframe dimensions are in the uncompressed header. Report them
    // before the decode worker runs, including frames rejected for size.
    if (keyframe && length >= 10 && frame->data[3] == 0x9d &&
        frame->data[4] == 0x01 && frame->data[5] == 0x2a) {
      const unsigned width = (frame->data[6] | (unsigned(frame->data[7]) << 8)) & 0x3fff;
      const unsigned height = (frame->data[8] | (unsigned(frame->data[9]) << 8)) & 0x3fff;
      if (width != state->input_video_width || height != state->input_video_height) {
        state->input_video_width = width;
        state->input_video_height = height;
        ESP_LOGI(kTag, "VP8 input dimensions=%ux%u keyframe_bytes=%u", width, height, unsigned(length));
      }
    }
  }
#if GEA_RTC_HAS_H264
  else {
  unsigned prefix = 0;
  for (size_t position = h264::startCode(frame->data, length, 0, prefix); position < length;) {
    const size_t nal = position + prefix;
    if (nal < length && (frame->data[nal] & 31) == 5) keyframe = true;
    if (nal < length && ((frame->data[nal] & 31) == 7 || (frame->data[nal] & 31) == 8)) headers = true;
    position = h264::startCode(frame->data, length, nal, prefix);
  }
  }
#endif
  std::lock_guard<std::mutex> lock(state->video_mutex);
  // A complete VP8 keyframe replaces the old reference chain. Skip queued
  // pictures from that old chain now, instead of spending seconds decoding
  // them before reaching an already-arrived independently decodable picture.
  // The single decode worker finishes its current picture before this one.
  if (state->vp8_video && keyframe && length >= 10 && frame->data[3] == 0x9d &&
      frame->data[4] == 0x01 && frame->data[5] == 0x2a && !state->video_frames.empty()) {
    ESP_LOGI(kTag, "VP8 newest keyframe replaces queued_references=%u", unsigned(state->video_frames.size()));
    state->video_frames.clear();
    state->request_video_keyframe = false;
  }
  // Compressed references are not a playback buffer. A keyframe can take
  // longer than three arrival intervals; repeatedly dropping its dependents
  // forces expensive keyframes forever. Retain bounded compressed references
  // while the worker coalesces obsolete decoded display images.
  size_t queuedBytes = 0;
  for (const auto& queued : state->video_frames) queuedBytes += queued.data.size();
  if (state->video_frames.size() >= 32 || queuedBytes + length > 512 * 1024) {
    state->video_frames.clear();
    state->need_video_keyframe = true;
    state->request_video_keyframe = true;
    ESP_LOGW(kTag, "video decoder behind; awaiting keyframe");
  }
  if (state->need_video_keyframe && !keyframe && !headers) return -1;
  if (keyframe) state->need_video_keyframe = false;
  try {
    state->video_frames.push_back({owned ? std::move(*owned)
        : EspPeerState::VideoBytes(frame->data, frame->data + length), frame->pts});
  } catch (...) {
    state->video_frames.clear(); state->need_video_keyframe = true;
    state->request_video_keyframe = true; return -1;
  }
  if (state->video_task) xTaskNotifyGive(state->video_task);
  return 0;
}

int on_video_data_cb(esp_peer_video_frame_t* frame, void* ctx) {
  return queue_video_data(frame, ctx);
}

#if GEA_RTC_HAS_VP8
int on_owned_video_data_cb(vp8::EncodedFrame&& owned, void* ctx) {
  esp_peer_video_frame_t frame{owned.timestamp / 90, owned.bytes.data(), int(owned.bytes.size())};
  return queue_video_data(&frame, ctx, &owned.bytes);
}
#endif

void video_decode_trampoline(void* opaque) {
  auto* state = static_cast<EspPeerState*>(opaque);
#if GEA_RTC_HAS_VP8 && defined(GEA_EMBEDDED_VP8_BENCHMARK) && GEA_EMBEDDED_VP8_BENCHMARK
  vp8::capture().workerStarted();
#endif
#if GEA_RTC_HAS_H264
  h264::Decoder decoder;
#endif
#if GEA_RTC_HAS_VP8
  vp8::Decoder vp8Decoder;
  ESP_LOGI(kTag, "VP8 internal workspace_bytes=%u", unsigned(vp8Decoder.internalWorkspaceBytes()));
#endif
  uint64_t sequence = 0;
  uint64_t referenceFrames = 0;
  unsigned completedOnCore[2]{};
  int64_t lastVideoReport = 0;
  [[maybe_unused]] int64_t lastVideoPresented = 0;
  while (state->open && !state->closing) {
    EspPeerState::EncodedVideo encoded;
    // Preserve codec references, but keep showing progress if decoding cannot
    // empty the queue. Waiting exclusively for an empty queue freezes video.
    [[maybe_unused]] bool moreQueued = false;
    {
      std::lock_guard<std::mutex> lock(state->video_mutex);
      if (!state->video_frames.empty()) {
        encoded = std::move(state->video_frames.front()); state->video_frames.pop_front();
        moreQueued = !state->video_frames.empty();
      }
    }
    if (encoded.data.empty()) { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20)); continue; }
    // A sustained reference backlog must not monopolize its core between frames.
    // One tick permits ready raster/idle work without dropping codec references
    // or delaying the higher-priority transport and audio tasks.
    struct FrameYield { ~FrameYield() { vTaskDelay(1); } } frameYield;
    try {
      const bool encodedKey = !(encoded.data.front() & 1);
      const auto encodedSize = encoded.data.size();
      std::shared_ptr<media::VideoFrame> frame;
      const auto decodeStarted = esp_timer_get_time();
      const auto decodeCore = xPortGetCoreID();
      const auto decodeCpuStarted = currentTaskCpuTicks();
#if GEA_RTC_HAS_VP8
      if (state->vp8_video) {
        const auto presentationDelay = moreQueued && sequence
            ? uint64_t(std::max<int64_t>(0, 100000 - (decodeStarted - lastVideoPresented))) : 0;
        frame = vp8Decoder.decode(encoded.data, encoded.pts, true, presentationDelay);
        const auto allocation = vp8Decoder.timing();
        if (allocation.allocationFailedWidth) {
          ESP_LOGW(kTag, "VP8 presentation allocation failed %ux%u bytes=%u psram_free=%u psram_largest=%u",
              allocation.allocationFailedWidth, allocation.allocationFailedHeight,
              allocation.allocationFailedWidth * allocation.allocationFailedHeight * 2,
              unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
              unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
        }
        if (!vp8Decoder.needsKeyframe()) ++referenceFrames;
        if (!frame && vp8Decoder.needsKeyframe()) {
          ESP_LOGW(kTag, "VP8 rejected frame bytes=%u key=%d elapsed_us=%lld",
              unsigned(encoded.data.size()), !(encoded.data[0] & 1),
              (long long)(esp_timer_get_time() - decodeStarted));
          std::lock_guard<std::mutex> lock(state->video_mutex);
          state->video_frames.clear(); state->need_video_keyframe = true;
          state->request_video_keyframe = true;
        }
      }
#endif
#if GEA_RTC_HAS_H264
      if (!state->vp8_video) frame = decoder.decode(encoded.data.data(), encoded.data.size(), encoded.pts);
#endif
      const auto decodedAt = esp_timer_get_time();
      const auto finishedCore = xPortGetCoreID();
      ++completedOnCore[finishedCore];
      const auto decodeCpu = configRUN_TIME_COUNTER_TYPE(currentTaskCpuTicks() - decodeCpuStarted);
#if GEA_RTC_HAS_VP8 && defined(GEA_EMBEDDED_VP8_BENCHMARK) && GEA_EMBEDDED_VP8_BENCHMARK
      // Retain the accepted reference prefix, including pictures coalesced at
      // presentation. Capture only after reading all metadata from encoded.
      if (state->vp8_video && !vp8Decoder.needsKeyframe() && vp8::capture().requested()) {
        const auto timing = vp8Decoder.timing();
        vp8::capture().take(encoded.data, encoded.pts, decodedAt - decodeStarted,
                           decodeCpu, timing.decodeUs, timing.convertUs);
      }
#endif
      if (!frame) continue;
      const bool reportVideo = !sequence || decodedAt - lastVideoReport >= 2000000;
      if (reportVideo) ESP_LOGI(kTag, "%s decode+RGB565=%lld us %ux%u frames=%llu cpu_us=%llu key=%u bytes=%u references=%llu",
          state->vp8_video ? "VP8" : "H264", (long long)(esp_timer_get_time() - decodeStarted),
          unsigned(frame->width), unsigned(frame->height), (unsigned long long)(sequence + 1),
          (unsigned long long)decodeCpu, unsigned(encodedKey), unsigned(encodedSize),
          (unsigned long long)referenceFrames);
      if (reportVideo) {
        ESP_LOGI(kTag, "video worker started_core=%d finished_core=%d completed_core0=%u completed_core1=%u",
            int(decodeCore), int(finishedCore), completedOnCore[0], completedOnCore[1]);
        completedOnCore[0] = completedOnCore[1] = 0;
      }
#if GEA_RTC_HAS_VP8
      if (state->vp8_video && reportVideo) {
        const auto timing = vp8Decoder.timing();
        ESP_LOGI(kTag, "VP8 setup_us=%llu codec_us=%llu rgb565_us=%llu psram_free=%u psram_largest=%u",
            (unsigned long long)timing.setupUs, (unsigned long long)timing.decodeUs,
            (unsigned long long)timing.convertUs,
            unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
            unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
        if (timing.macroblocks) ESP_LOGI(kTag,
            "VP8 stages wall_us: tokens=%llu reconstruction=%llu loopfilter=%llu other=%llu macroblocks=%u token_blocks=%u filter_rows=%u",
            (unsigned long long)timing.tokenUs,
            (unsigned long long)(timing.macroblockUs >= timing.tokenUs ? timing.macroblockUs - timing.tokenUs : 0),
            (unsigned long long)timing.filterUs,
            (unsigned long long)(timing.decodeUs >= timing.macroblockUs + timing.filterUs ? timing.decodeUs - timing.macroblockUs - timing.filterUs : 0),
            timing.macroblocks, timing.tokenBlocks, timing.filterRows);
      }
#endif
      if (reportVideo) lastVideoReport = decodedAt;
      if (!state->remote_video_track) {
        state->remote_video_stream = media::create_remote_video_stream();
        state->remote_video_track = MediaStream(state->remote_video_stream).getVideoTracks().front().nativeHandle;
        enqueue_track(state->our_handle, state->remote_video_track);
      }
      frame->sequence = ++sequence;
      media::publish_video_frame(state->remote_video_track, std::move(frame));
      lastVideoPresented = esp_timer_get_time();
    } catch (...) {
      ESP_LOGE(kTag, "Video decode/publish failed: internal_free=%u psram_free=%u psram_largest=%u",
          unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
          unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
          unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
#if GEA_RTC_HAS_H264
      decoder.reset();
#endif
#if GEA_RTC_HAS_VP8
      vp8Decoder.reset();
#endif
      std::lock_guard<std::mutex> lock(state->video_mutex);
      state->video_frames.clear(); state->need_video_keyframe = true;
      state->request_video_keyframe = true;
    }
  }
#if GEA_RTC_HAS_H264
  decoder.reset();
#endif
#if GEA_RTC_HAS_VP8
  vp8Decoder.reset();
#endif
#if GEA_RTC_HAS_VP8 && defined(GEA_EMBEDDED_VP8_BENCHMARK) && GEA_EMBEDDED_VP8_BENCHMARK
  vp8::capture().workerStopped();
#endif
  xSemaphoreGive(state->video_done);
  for (;;) vTaskSuspend(nullptr);
}
#endif

void main_loop_trampoline(void *arg) {
  auto *peer_state = static_cast<EspPeerState *>(arg);
  int64_t measured_at = esp_timer_get_time(), loop_us = 0, max_loop_us = 0;
  auto cpuStart = currentTaskCpuTicks();
  uint32_t loop_count = 0;
  while (peer_state->open && !peer_state->closing) {
    const auto loop_start = esp_timer_get_time();
    esp_peer_main_loop(peer_state->handle);
    const auto elapsed = esp_timer_get_time() - loop_start;
    loop_us += elapsed;
    max_loop_us = std::max(max_loop_us, elapsed);
    ++loop_count;
    if (esp_timer_get_time() - measured_at >= 2000000) {
      const auto cpuNow = currentTaskCpuTicks();
      ESP_LOGI(kTag, "transport span_ms=%lld calls=%u loop_us=%lld decode_us=%u publish_us=%u decoded=%u audio_queue_drops=%u max_loop_us=%lld peer=%u cpu_us=%llu",
        (long long)((esp_timer_get_time() - measured_at) / 1000), unsigned(loop_count),
        (long long)loop_us, unsigned(peer_state->decode_time_us.exchange(0)),
        unsigned(peer_state->publish_time_us.exchange(0)), unsigned(peer_state->decode_count.exchange(0)),
        unsigned(peer_state->received_audio_drops.load()), (long long)max_loop_us,
        unsigned(peer_state->our_handle),
        (unsigned long long)configRUN_TIME_COUNTER_TYPE(cpuNow - cpuStart));
      cpuStart = cpuNow;
      measured_at = esp_timer_get_time(); loop_us = max_loop_us = 0; loop_count = 0;
    }
    // SCTP association must be established before DCEP creates channels.
    // DTLS/SRTP connected is an earlier, separate transport event.
    if (peer_state->data_connected) {
      std::lock_guard<std::mutex> lock(peer_state->channels_mutex);
      for (auto it = peer_state->pending_channels.begin(); it != peer_state->pending_channels.end();) {
        esp_peer_data_channel_cfg_t cfg{};
        cfg.label = it->first.data(); cfg.ordered = it->second.ordered;
        cfg.type = ESP_PEER_DATA_CHANNEL_RELIABLE;
        if (it->second.maxRetransmits) { cfg.type = ESP_PEER_DATA_CHANNEL_PARTIAL_RELIABLE_RETX; cfg.max_retransmit_count = *it->second.maxRetransmits; }
        else if (it->second.maxPacketLifeTime) { cfg.type = ESP_PEER_DATA_CHANNEL_PARTIAL_RELIABLE_TIMEOUT; cfg.max_packet_lifetime = *it->second.maxPacketLifeTime; }
        const int result = esp_peer_create_data_channel(peer_state->handle, &cfg);
        if (result == ESP_PEER_ERR_WRONG_STATE || result == ESP_PEER_ERR_WOULD_BLOCK) { ++it; continue; }
        if (result != 0) { ESP_LOGE(kTag, "Data channel creation failed: %d", result); enqueue_connection_state_change(peer_state->our_handle, "failed"); }
        it = peer_state->pending_channels.erase(it);
      }
    }
    // Native Daily transports wake on incoming datagrams and encoded mic
    // packets. Idle polling at 1 ms consumed ~10% of a core on the send peer
    // alone. A 10 ms timer still services loss recovery/RTCP when no packet
    // arrives; it does not add a media prefill or delay an arriving packet.
    if (peer_state->packet_wake)
      xSemaphoreTake(peer_state->transport_wake, pdMS_TO_TICKS(10));
    else
      vTaskDelay(pdMS_TO_TICKS(1));
  }
  xSemaphoreGive(peer_state->main_done);
  for (;;) vTaskSuspend(nullptr); // Owner frees the PSRAM stack after joining.
}

void audio_send_trampoline(void *arg) {
  auto *peer_state = static_cast<EspPeerState *>(arg);
  alignas(16) std::int16_t buffer[kSendFrameSamples];
  size_t filled = 0;
  NativeMediaTrackHandle previous_track = 0;
  std::optional<gea::host::media::TrackPcmReader> microphone;
  uint32_t timestamp_ms = 0;
  bool reported_stack = false;
  int64_t window_start = esp_timer_get_time(), encode_us = 0, max_encode_us = 0;
  int64_t send_us = 0, max_send_us = 0;
  uint32_t sent_frames = 0, send_errors = 0;
  uint32_t encoded_on_core[2]{};
  auto cpuStart = currentTaskCpuTicks();
  while (peer_state->open && !peer_state->closing) {
    const auto track = peer_state->local_track.load();
    if (!track || !peer_state->connected) {
      microphone.reset();
      previous_track = 0;
      filled = 0;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (track != previous_track) {
      // Subscribe at live capture only after the transport connects. The
      // compatibility cursor includes up to two seconds of preconnection PCM.
      microphone.emplace(track, false);
      previous_track = track;
      filled = 0;
      window_start = esp_timer_get_time();
      cpuStart = currentTaskCpuTicks();
      encode_us = max_encode_us = 0;
      send_us = max_send_us = 0;
      sent_frames = send_errors = 0;
      peer_state->received_audio_frames.store(0);
    }
    filled += microphone->read(buffer + filled, kSendFrameSamples - filled);
    if (filled < kSendFrameSamples) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
    const int64_t encode_start = esp_timer_get_time();
    ++encoded_on_core[xPortGetCoreID()];
    const int bytes = peer_state->audio->encode(buffer);
    const int64_t encoded_us = esp_timer_get_time() - encode_start;
    encode_us += encoded_us;
    if (encoded_us > max_encode_us) max_encode_us = encoded_us;
    if (!reported_stack && bytes > 0) {
      reported_stack = true;
      ESP_LOGI(kTag, "first encoded audio: bytes=%d, worker stack free=%u/%u", bytes,
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)), static_cast<unsigned>(kSendStackBytes));
    }
    filled = 0;
    if (bytes < 0) { ESP_LOGE(kTag, "Opus encode failed: %d", bytes); continue; }
    esp_peer_audio_frame_t frame = {};
    frame.pts = timestamp_ms;
    frame.data = peer_state->audio->packet.data();
    frame.size = bytes;
    timestamp_ms += OpusAudio::frameDurationMs;
    const auto send_start = esp_timer_get_time();
    const int err = esp_peer_send_audio(peer_state->handle, &frame);
    const auto send_elapsed = esp_timer_get_time() - send_start;
    send_us += send_elapsed;
    max_send_us = std::max(max_send_us, send_elapsed);
    if (err != 0) { ++send_errors; ESP_LOGW(kTag, "Audio send failed: %d", err); }
    else ++sent_frames;
    const int64_t now = esp_timer_get_time();
    if (now - window_start >= 2000000) {
      const auto cpuNow = currentTaskCpuTicks();
      ESP_LOGI(kTag, "duplex span_ms=%lld mic_sent=%u remote_received=%u send_errors=%u encode_avg_us=%lld encode_max_us=%lld mic_pending_ms=%u mic_dropped=%llu encode_core0=%u encode_core1=%u remote_opus_config=%u local_opus_config=%u send_avg_us=%lld send_max_us=%lld cpu_us=%llu",
        static_cast<long long>((now - window_start) / 1000), unsigned(sent_frames),
        unsigned(peer_state->received_audio_frames.exchange(0)), unsigned(send_errors),
        static_cast<long long>(encode_us / (sent_frames + send_errors ? sent_frames + send_errors : 1)),
        static_cast<long long>(max_encode_us),
        unsigned(microphone->pendingSamples() * 1000 / OpusAudio::sampleRate),
        static_cast<unsigned long long>(microphone->droppedSamples()),
        unsigned(encoded_on_core[0]), unsigned(encoded_on_core[1]),
        peer_state->received_opus_config.load(std::memory_order_relaxed), unsigned(peer_state->audio->packet[0] >> 3),
        (long long)(send_us / (sent_frames + send_errors ? sent_frames + send_errors : 1)), (long long)max_send_us,
        (unsigned long long)configRUN_TIME_COUNTER_TYPE(cpuNow - cpuStart));
      cpuStart = cpuNow;
// Never take a task census here: uxTaskGetSystemState scans every task's
      // stack while holding the FreeRTOS kernel lock, delaying DMA/network work.

      window_start = now; encode_us = max_encode_us = 0; sent_frames = send_errors = 0;
      send_us = max_send_us = 0;
      encoded_on_core[0] = encoded_on_core[1] = 0;
    }
  }
  microphone.reset(); // Release the reader before this task is suspended/deleted.
  xSemaphoreGive(peer_state->audio_done);
  for (;;) vTaskSuspend(nullptr);
}

void releaseTransport(EspPeerState* state);

EspPeerState *ensurePeer(EspPeerState* state, bool answer = false) {
  if (!state) return nullptr;
  if (state->closing) throw std::runtime_error("AbortError: peer is closing");
  if (state->handle) return state;
  // Gea initializes lwIP only when Wi-Fi is enabled. Opening sockets sooner
  // asserts in tcpip_send_msg_wait_sem; propagate a normal JS connection error.
  if (!gea::framework::network::wifi().connected())
    throw std::runtime_error("Wi-Fi is not connected. Check the device network settings and try Start again.");
  try {
  ESP_LOGI(kTag, "opening transport");
  const auto opusDirection = state->direction == "sendonly" ? OpusAudio::Direction::Encode :
      state->direction == "recvonly" ? OpusAudio::Direction::Decode : OpusAudio::Direction::Both;
  const auto internalBeforeOpus = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  state->audio = std::make_unique<OpusAudio>(OpusAudio::Application::LowDelay, opusDirection);
  ESP_LOGI(kTag, "Opus direction=%s internal_before=%u internal_after=%u", state->direction.c_str(),
      unsigned(internalBeforeOpus), unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
  state->sdp_ready = xSemaphoreCreateBinary();
  state->main_done = xSemaphoreCreateBinary();
  state->audio_done = xSemaphoreCreateBinary();
  if (!state->sdp_ready || !state->main_done || !state->audio_done) throw std::runtime_error("RTC semaphore allocation failed");
  for (auto& server : state->config.iceServers) for (auto& url : server.urls) {
    state->servers.push_back({url.data(),
      server.username.empty() ? nullptr : server.username.data(),
      server.credential.empty() ? nullptr : server.credential.data()});
  }
  if (state->servers.size() > 255) throw std::invalid_argument("Too many ICE servers");
  state->open = true;
  esp_peer_cfg_t cfg = {};
  // Keep the transport's default receive timeout: agent_recv_timeout also
  // governs DTLS handshakes. A 5 ms override caused repeated -0x7880 timeouts
  // and prevented an accepted OpenAI session from ever connecting.
  cfg.server_lists = state->servers.data();
  cfg.server_num = static_cast<uint8_t>(state->servers.size());
  cfg.role = answer ? ESP_PEER_ROLE_CONTROLLED : ESP_PEER_ROLE_CONTROLLING;
  cfg.ice_trans_policy = state->config.iceTransportPolicy == "relay" ? ESP_PEER_ICE_TRANS_POLICY_RELAY : ESP_PEER_ICE_TRANS_POLICY_ALL;
  cfg.audio_info.codec = ESP_PEER_AUDIO_CODEC_OPUS;
  cfg.audio_info.sample_rate = kOpusRtpClockRate;
  cfg.audio_info.channel = kOpusRtpChannels;
  cfg.audio_dir = state->direction == "sendonly" ? ESP_PEER_MEDIA_DIR_SEND_ONLY :
                  state->direction == "recvonly" ? ESP_PEER_MEDIA_DIR_RECV_ONLY :
                  state->direction == "inactive" ? ESP_PEER_MEDIA_DIR_NONE : ESP_PEER_MEDIA_DIR_SEND_RECV;
  if (state->direction == "recvonly" || state->direction == "sendrecv") {
    state->audio_receive_done = xSemaphoreCreateBinary();
    // Decode each authenticated audio packet before the transport drains its
    // next burst. Running below the RTP worker lets startup bursts fill the
    // bounded queue even when steady-state decoding meets its deadline.
    if (!state->audio_receive_done ||
        xTaskCreatePinnedToCoreWithCaps(audio_receive_trampoline, "gea_rtc_audio", kPeerStackBytes,
            state, 8, &state->audio_receive_task, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
      throw std::runtime_error("RTC audio decoder task allocation failed");
  }
  cfg.video_dir = ESP_PEER_MEDIA_DIR_NONE;
#if GEA_RTC_HAS_VIDEO
  if (state->video_direction == "recvonly") {
    cfg.video_dir = ESP_PEER_MEDIA_DIR_RECV_ONLY;
    cfg.video_info.codec = ESP_PEER_VIDEO_CODEC_H264;
#if GEA_RTC_HAS_VP8
    if (state->vp8_video) cfg.video_info.codec = GEA_PEER_VIDEO_CODEC_VP8;
#endif
    cfg.on_video_data = on_video_data_cb;
    state->video_done = xSemaphoreCreateBinary();
    if (!state->video_done) throw std::runtime_error("Video semaphore allocation failed");
    // IDF pins coprocessor users, including PIE SIMD, on first use. Keep VP8
    // on CPU1: CPU0 capture/send/UI work starved it in live measurements.
    // Run VP8 above raster work (3), but below audio decode (8), transport (7)
    // and audio send (5). A per-frame tick below lets raster/idle work progress.
    // The vendor H264 decoder keeps its existing affinity.
    if (xTaskCreatePinnedToCoreWithCaps(video_decode_trampoline, "gea_rtc_video", 64 * 1024, state,
                                      state->vp8_video ? 4 : 3, &state->video_task, 1,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
      throw std::runtime_error("Video task allocation failed");
  }
#endif
  { std::lock_guard<std::mutex> lock(state->channels_mutex);
    cfg.enable_data_channel = !state->pending_channels.empty(); }
  cfg.manual_ch_create = true;
  cfg.ctx = state;
  cfg.on_state = on_state_cb;
  cfg.on_msg = on_msg_cb;
  cfg.on_audio_data = on_audio_data_cb;
  cfg.on_channel_open = [](esp_peer_data_channel_info_t* channel, void* ctx) -> int {
    auto* peer = static_cast<EspPeerState*>(ctx);
    enqueue_channel_open(peer->our_handle, channel->label ? channel->label : "", channel->stream_id); return 0;
  };
  cfg.on_channel_close = [](esp_peer_data_channel_info_t* channel, void* ctx) -> int {
    enqueue_channel_close(static_cast<EspPeerState*>(ctx)->our_handle, channel->stream_id); return 0;
  };
  cfg.on_data = [](esp_peer_data_frame_t* frame, void* ctx) -> int {
    if (frame && frame->size >= 0) enqueue_channel_message(static_cast<EspPeerState*>(ctx)->our_handle,
      frame->stream_id, frame->data, static_cast<size_t>(frame->size), frame->type == ESP_PEER_DATA_CHANNEL_STRING);
    return 0;
  };

  const auto* implementation = esp_peer_get_default_impl();
#if GEA_RTC_HAS_VP8
  GeaVp8PeerOptions vp8Options{&state->open, &state->request_video_keyframe};
  if (state->native_daily_receive || state->vp8_video || (state->direction == "sendonly" &&
      cfg.video_dir == ESP_PEER_MEDIA_DIR_NONE && !cfg.enable_data_channel &&
      state->servers.empty() &&
      cfg.ice_trans_policy != ESP_PEER_ICE_TRANS_POLICY_RELAY)) {
    implementation = gea_vp8_peer_impl();
    state->transport_wake = xSemaphoreCreateBinary();
    if (!state->transport_wake) throw std::runtime_error("RTC transport wake allocation failed");
    state->packet_wake = true;
    vp8Options.wake = [](void* context) {
      auto* peer = static_cast<EspPeerState*>(context);
      xSemaphoreGive(peer->transport_wake);
    };
    vp8Options.wakeContext = state;
    vp8Options.onOwnedVideo = on_owned_video_data_cb;
    cfg.extra_cfg = &vp8Options;
    cfg.extra_size = sizeof(vp8Options);
  }
#endif
  int err = esp_peer_open(&cfg, implementation, &state->handle);
  if (err != 0 || !state->handle) {
    ESP_LOGE(kTag, "esp_peer_open failed: %d", err);
    state->open = false;
    throw std::runtime_error("esp_peer_open failed");
  }

  // These are network/codec workers, not DMA buffers or interrupt stacks.
  // Keep their stacks in PSRAM, like the existing HTTP worker pool.
  // Separate the native VP8 receive transport from its decoder. Keep receive
  // audio on CPU1: moving both overloaded CPU0 and delayed microphone encoding.
  // On CPU0 transport must stay below capture/send (24/5), above UI (3).
  // Other peers retain their established scheduling.
  const bool separateVideoTransport = state->vp8_video && state->packet_wake;
  if (xTaskCreatePinnedToCoreWithCaps(main_loop_trampoline, "gea_rtc_loop", kPeerStackBytes, state,
                                   separateVideoTransport ? 4 : 7, &state->main_loop_task,
                                   separateVideoTransport ? 0 : 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS ||
      // Capture and encode share CPU0; receive/decode use CPU1. Keeping both
      // Opus directions on CPU1 starves video even when CPU0 has spare time.
      // RGB panels must use DMA scanout rather than continuous CPU bounce
      // copies to leave enough CPU0 time for capture/AEC and encoding.
      // NO_AFFINITY is not stable here: Xtensa's first FPU instruction pins
      // an unpinned task to whichever core happened to execute it.
      ((state->direction == "sendonly" || state->direction == "sendrecv") &&
       xTaskCreatePinnedToCoreWithCaps(audio_send_trampoline, "gea_rtc_send", kSendStackBytes, state, 5, &state->audio_send_task, 0,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS))
    throw std::runtime_error("RTC task allocation failed");
  ESP_LOGI(kTag, "transport tasks started");
  return state;
  } catch (const std::exception& error) {
    ESP_LOGE(kTag, "transport initialization failed: %s (internal free=%u largest=%u)", error.what(),
      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    releaseTransport(state); throw;
  } catch (...) { releaseTransport(state); throw; }
}

EspPeerState *ensurePeer(NativeRtcPeerHandle id, bool answer = false) {
  return ensurePeer(findPeer(id), answer);
}

void releaseTransport(EspPeerState *state) {
  state->open = false;
  if (state->transport_wake) xSemaphoreGive(state->transport_wake);
  // Both tasks can still be using the peer and its SDP semaphore. Join them
  // before closing the transport or freeing state (the old code freed first).
  const auto join = [](TaskHandle_t& task, SemaphoreHandle_t done) {
    if (!task) return;
    ESP_LOGI(kTag, "joining %s", pcTaskGetName(task));
    while (xSemaphoreTake(done, pdMS_TO_TICKS(2000)) != pdTRUE)
      ESP_LOGW(kTag, "waiting for %s state=%d", pcTaskGetName(task), static_cast<int>(eTaskGetState(task)));
    // The completion signal precedes suspension. Do not free a stack while
    // its task is still executing on the other core.
    while (eTaskGetState(task) != eSuspended) vTaskDelay(1);
    ESP_LOGI(kTag, "joined %s", pcTaskGetName(task));
    vTaskDeleteWithCaps(task);
    task = nullptr;
  };
  join(state->main_loop_task, state->main_done);
  join(state->audio_send_task, state->audio_done);
  join(state->audio_receive_task, state->audio_receive_done);
  if (state->audio_receive_done) vSemaphoreDelete(state->audio_receive_done);
  state->audio_receive_done = nullptr;
  state->received_audio_queue.clear();
#if GEA_RTC_HAS_VIDEO
  join(state->video_task, state->video_done);
  if (state->video_done) vSemaphoreDelete(state->video_done);
  state->video_done = nullptr;
  state->video_frames.clear();
#endif
  if (state->handle) {
    ESP_LOGI(kTag, "closing peer transport");
    esp_peer_close(state->handle);
    ESP_LOGI(kTag, "closed peer transport");
    state->handle = nullptr;
  }
  if (state->sdp_ready) {
    vSemaphoreDelete(state->sdp_ready);
    state->sdp_ready = nullptr;
  }
  if (state->main_done) vSemaphoreDelete(state->main_done);
  // esp_peer_close joins ICE callbacks before this semaphore is freed.
  if (state->transport_wake) vSemaphoreDelete(state->transport_wake);
  state->transport_wake = nullptr;
  state->packet_wake = false;
  if (state->audio_done) vSemaphoreDelete(state->audio_done);
  state->main_done = nullptr; state->audio_done = nullptr;
  if (state->remote_stream) gea::host::media::destroy_stream(state->remote_stream);
  if (state->remote_video_stream) media::destroy_stream(state->remote_video_stream);
  state->remote_video_stream = 0; state->remote_video_track = 0;
  state->remote_stream = 0; state->remote_track = 0;
  state->audio.reset(); state->servers.clear();
}

}  // namespace

void platform_create(NativeRtcPeerHandle id, const RTCConfiguration& config) {
  ensureControlTask();
  auto state = std::make_unique<EspPeerState>();
  state->our_handle = id; state->config = config;
  std::lock_guard<std::mutex> lock(espPeerMutex());
  espPeerTable()[id] = state.release();
}

void platform_destroy(NativeRtcPeerHandle our_handle) {
  EspPeerState* state;
  {
    std::lock_guard<std::mutex> lock(espPeerMutex());
    const auto it = espPeerTable().find(our_handle);
    if (it == espPeerTable().end()) return;
    state = it->second;
    state->closing = true;
    state->open = false;
    // Enqueue before erasing so a failed allocation leaves ownership intact.
    queueControl([state] { releaseTransport(state); delete state; });
    espPeerTable().erase(it);
  }
}

void platform_request_description(NativeRtcPeerHandle id, bool offer, std::shared_ptr<DescriptionResult> result) {
  auto* state = findPeer(id);
  if (!state) throw std::runtime_error("InvalidStateError: peer closed");
  queueControl([state, offer, result] {
    try {
      ensurePeer(state, !offer);
      if (offer && esp_peer_new_connection(state->handle) != 0)
        throw std::runtime_error("OperationError: ICE offer failed");
      const auto deadline = esp_timer_get_time() + 5000000;
      while (!state->closing && esp_timer_get_time() < deadline) {
        if (xSemaphoreTake(state->sdp_ready, pdMS_TO_TICKS(20)) == pdTRUE) {
          std::lock_guard<std::mutex> lock(state->sdp_mutex);
          result->sdp = state->pending_sdp;
          break;
        }
      }
      if (state->closing) result->error = "AbortError: peer closed during SDP creation";
      else if (result->sdp.empty()) result->error = "OperationError: SDP creation timed out";
    } catch (const std::exception& error) { result->error = error.what(); }
    catch (...) { result->error = "OperationError: SDP creation failed"; }
    result->ready.store(true, std::memory_order_release);
  });
}

void platform_add_track(NativeRtcPeerHandle our_handle, NativeMediaTrackHandle track) {
  auto *state = findPeer(our_handle);
  if (!state) throw std::runtime_error("InvalidStateError: peer closed");
  state->local_track = track;
}

std::string platform_create_offer(NativeRtcPeerHandle our_handle) {
  auto *state = ensurePeer(our_handle);
  if (!state) return std::string();
  ESP_LOGI(kTag, "starting ICE offer");
  int err = esp_peer_new_connection(state->handle);
  ESP_LOGI(kTag, "ICE offer returned %d", err);
  if (err != 0) {
    ESP_LOGE(kTag, "esp_peer_new_connection failed: %d", err);
    return std::string();
  }
  // Block up to 5 s for on_msg(SDP) to fire. Caller (JS code under await) is on a
  // different task than the main_loop_task running the callback.
  if (xSemaphoreTake(state->sdp_ready, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(kTag, "create_offer SDP timeout");
    return std::string();
  }
  std::lock_guard<std::mutex> lock(state->sdp_mutex);
  return state->pending_sdp;
}

std::string platform_create_answer(NativeRtcPeerHandle our_handle) {
  // esp_peer auto-generates the answer on first inbound offer through send_msg.
  // Block until on_msg fires with the locally-generated SDP.
  auto *state = ensurePeer(our_handle);
  if (!state) return std::string();
  if (xSemaphoreTake(state->sdp_ready, pdMS_TO_TICKS(5000)) != pdTRUE) {
    return std::string();
  }
  std::lock_guard<std::mutex> lock(state->sdp_mutex);
  return state->pending_sdp;
}

void platform_set_local_description(NativeRtcPeerHandle, const std::string &, const std::string &) {
  // esp_peer treats the SDP returned from on_msg as already the local description.
  // No explicit set is required.
}

void platform_set_remote_description(NativeRtcPeerHandle our_handle, const std::string &type, const std::string &sdp) {
#if GEA_RTC_HAS_VP8
  if (type == "offer") {
    if (auto* pending = findPeer(our_handle); pending && !pending->handle) {
      const auto offer = receive::parseOffer(sdp);
      pending->native_daily_receive = bool(offer);
      pending->vp8_video = offer && std::any_of(offer->tracks.begin(), offer->tracks.end(),
          [](const auto& track) { return track.kind == "video"; });
    }
  }
#endif
  auto *state = ensurePeer(our_handle, type == "offer");
  if (!state || !state->handle) return;
  for (const auto* prefix : {"a=setup:", "m=application ", "a=sctp-port:", "a=ice-lite"}) {
    for (std::size_t pos = sdp.find(prefix); pos != std::string::npos; pos = sdp.find(prefix, pos + 1)) {
      const auto end = sdp.find_first_of("\r\n", pos);
      ESP_LOGI(kTag, "remote %s", sdp.substr(pos, end - pos).c_str());
    }
  }
  esp_peer_msg_t msg = {};
  msg.type = ESP_PEER_MSG_TYPE_SDP;
  msg.data = reinterpret_cast<uint8_t *>(const_cast<char *>(sdp.c_str()));
  msg.size = static_cast<int>(sdp.size());
  if (esp_peer_send_msg(state->handle, &msg) != 0) throw std::runtime_error("OperationError: SDP/ICE update rejected");
}

void platform_add_ice_candidate(NativeRtcPeerHandle our_handle, const std::string &candidate, const std::string &, int) {
  auto *state = findPeer(our_handle);
  if (!state || !state->handle) return;
  esp_peer_msg_t msg = {};
  msg.type = ESP_PEER_MSG_TYPE_CANDIDATE;
  msg.data = reinterpret_cast<uint8_t *>(const_cast<char *>(candidate.c_str()));
  msg.size = static_cast<int>(candidate.size());
  if (esp_peer_send_msg(state->handle, &msg) != 0) throw std::runtime_error("OperationError: SDP/ICE update rejected");
}

void platform_direction(NativeRtcPeerHandle id, const std::string& kind, const std::string& direction) {
  auto* state = findPeer(id);
  if (!state) throw std::runtime_error("InvalidStateError: peer closed");
  if (kind == "video" && direction != "inactive" && (!GEA_RTC_HAS_VIDEO || direction != "recvonly"))
    throw std::runtime_error("NotSupportedError: native video reception is unavailable");
  auto& current = kind == "video" ? state->video_direction : state->direction;
  if (state->handle && current != direction)
    throw std::runtime_error("NotSupportedError: esp_peer cannot renegotiate media direction on an open peer");
  current = direction;
}
void platform_create_channel(NativeRtcPeerHandle id, const std::string& label, const RTCDataChannelInit& config) {
  auto* state = findPeer(id);
  if (!state) throw std::runtime_error("InvalidStateError: peer closed");
  std::lock_guard<std::mutex> lock(state->channels_mutex);
  state->pending_channels.emplace_back(label, config);
}
void platform_send_data(NativeRtcPeerHandle id, uint16_t stream, const uint8_t* bytes, size_t size, bool text) {
  auto* state = findPeer(id);
  if (!state || !state->handle || !state->connected) throw std::runtime_error("InvalidStateError: peer not connected");
  if (size > INT_MAX) throw std::invalid_argument("Data channel message too large");
  esp_peer_data_frame_t frame{};
  frame.stream_id = stream; frame.data = const_cast<uint8_t*>(bytes); frame.size = size;
  frame.type = text ? ESP_PEER_DATA_CHANNEL_STRING : ESP_PEER_DATA_CHANNEL_DATA;
  if (esp_peer_send_data(state->handle, &frame) != 0) throw std::runtime_error("OperationError: data channel send failed");
}
void platform_close_channel(NativeRtcPeerHandle id, const std::string& label) {
  auto* state = findPeer(id);
  if (!state) return;
  { std::lock_guard<std::mutex> lock(state->channels_mutex);
    for (auto it = state->pending_channels.begin(); it != state->pending_channels.end(); ++it) {
      if (it->first == label) { state->pending_channels.erase(it); return; }
    }
  }
  if (state->handle) {
    // esp_peer forwards SCTP's RE_CONFIG write result: a positive byte count
    // is success too. Treating it as failure strands JS cleanup in Closing.
    const int result = esp_peer_close_data_channel(state->handle, label.c_str());
    ESP_LOGI(kTag, "data channel close result=%d", result);
    if (result < 0) throw std::runtime_error("OperationError: data channel close failed");
  }
}

void platform_close(NativeRtcPeerHandle our_handle) {
  platform_destroy(our_handle);
}

}  // namespace gea::host::rtc

#endif  // ESP_PLATFORM
