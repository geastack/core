// SPDX-License-Identifier: Apache-2.0
#include "host/mjpeg.h"
#include "host/mjpeg_parser.h"
#include "host/h264_packet_queue.h"
#include "host/video_frame_pool.h"
#include "pixel.h"
#include <atomic>
#include <mutex>
#include <optional>
#include <stdexcept>

// The generated application archive is linked after managed prebuilt codecs.
// Resolve this object before those libraries (as with the GPIO bindings), so
// the late HTMLVideoElement caller does not strand JPEG symbols behind them.
extern "C" void gea_mjpeg_link_bindings() {}

#if defined(ESP_PLATFORM) && !defined(GEA_EMBEDDED_WIFI_DISABLED) && __has_include("esp_jpeg_dec.h")
#include "esp_http_client.h"
#include "esp_jpeg_dec.h"
#if __has_include("esp_h264_dec_sw.h")
#include "host/rtc_h264.h"
#define GEA_HAS_MULTIPART_H264 1
#endif
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#if GEA_EMBEDDED_ENABLE_HTTPS
#include "esp_crt_bundle.h"
#endif
#define GEA_HAS_MJPEG_STREAM 1
#endif

namespace gea::host::video {
struct MjpegStream::State {
  MediaStream source{media::create_remote_video_stream()};
  std::atomic<bool> running{true};
  ~State() {
#ifdef GEA_HAS_MJPEG_STREAM
    if (wake) vSemaphoreDelete(wake);
#endif
    media::destroy_stream(source.nativeHandle);
  }
#ifdef GEA_HAS_MJPEG_STREAM
  std::string url;
  SemaphoreHandle_t wake = nullptr;
  std::mutex mutex;
  std::optional<JpegPacket> newest;
  H264PacketQueue h264Packets;
  std::atomic<unsigned> received{0}, replaced{0};

  void receive() {
    esp_http_client_config_t config{};
    config.url = url.c_str();
    // Initial ARP/TCP setup can exceed one second on a newly booted station.
    // The steady-state read timeout is shortened after the connection opens.
    config.timeout_ms = 5000;
    config.buffer_size = 4096;
    config.disable_auto_redirect = true;
#if GEA_EMBEDDED_ENABLE_HTTPS
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    auto client = esp_http_client_init(&config);
    if (!client) { ESP_LOGE("gea_mjpeg", "MJPEG HTTP allocation failed"); return; }
    struct Cleanup {
      esp_http_client_handle_t client;
      ~Cleanup() { esp_http_client_cleanup(client); }
    } cleanup{client};
    if (esp_http_client_open(client, 0) != ESP_OK ||
        esp_http_client_fetch_headers(client) < 0 || esp_http_client_get_status_code(client) != 200) {
      ESP_LOGE("gea_mjpeg", "MJPEG HTTP connection rejected");
      return;
    }
    esp_http_client_set_timeout_ms(client, 1000);
    MjpegParser parser;
    std::array<uint8_t, 4096> bytes{};
    auto lastData = esp_timer_get_time();
    bool receivedData = false;
    while (running) {
      const int size = esp_http_client_read(client, reinterpret_cast<char*>(bytes.data()), bytes.size());
      if (!running) break;
      if (size == -ESP_ERR_HTTP_EAGAIN && esp_timer_get_time() - lastData <
          (receivedData ? 15000000 : 150000000)) continue;
      if (size <= 0) { ESP_LOGE("gea_mjpeg", "MJPEG HTTP stream ended"); return; }
      lastData = esp_timer_get_time();
      receivedData = true;
      if (!parser.push(std::span(bytes.data(), size_t(size)), [&](JpegPacket packet) {
        {
          std::lock_guard lock(mutex);
          if (packet.h264) replaced += h264Packets.push(std::move(packet));
          else {
            if (newest) ++replaced;
            newest = std::move(packet);
          }
          ++received;
        }
        xSemaphoreGive(wake);
      })) { ESP_LOGE("gea_mjpeg", "MJPEG malformed or oversized multipart frame"); return; }
      // Receiving owns no decode/display work and cannot build a frame queue.
      taskYIELD();
    }
  }

  void decode() {
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = GEA_EMBEDDED_PIXEL_PANEL_ENDIAN ?
        JPEG_PIXEL_FORMAT_RGB565_BE : JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t decoder = nullptr;
    struct Cleanup {
      jpeg_dec_handle_t& decoder;
      ~Cleanup() { if (decoder) jpeg_dec_close(decoder); }
    } cleanup{decoder};
    media::VideoFramePool frames;
#ifdef GEA_HAS_MULTIPART_H264
    rtc::h264::Decoder h264Decoder;
#endif
    const auto track = source.getVideoTracks().front().nativeHandle;
    unsigned decoded = 0, raw = 0, h264 = 0;
    int64_t report = esp_timer_get_time(), totalDecode = 0, h264DecodeUs = 0, h264ConvertUs = 0;
    uint64_t workerCpuUs = 0;
    while (running) {
      xSemaphoreTake(wake, pdMS_TO_TICKS(100));
      if (!running) break;
      std::optional<JpegPacket> packet;
      {
        std::lock_guard lock(mutex);
        packet = h264Packets.pop();
        if (!packet) packet.swap(newest);
        // A binary semaphore must keep draining an existing reference queue
        // even if upstream pauses before sending another packet.
        if (!h264Packets.empty()) xSemaphoreGive(wake);
      }
      if (!packet) continue;
      const auto started = esp_timer_get_time();
#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
      TaskStatus_t cpuBefore{};
      vTaskGetInfo(nullptr, &cpuBefore, pdFALSE, eRunning);
#endif
      auto frame = std::move(packet->rawFrame);
      if (frame) {
        ++raw;
      } else if (packet->h264) {
#ifdef GEA_HAS_MULTIPART_H264
        if (packet->resetDecoder) h264Decoder.reset();
        frame = h264Decoder.decode(packet->bytes.data(), packet->bytes.size(), packet->timestampMs);
        if (!frame) throw std::runtime_error("H264 access unit decode failed");
        h264DecodeUs += h264Decoder.lastDecodeUs();
        h264ConvertUs += h264Decoder.lastConvertUs();
        ++h264;
        if (h264 == 1) ESP_LOGI("gea_h264", "simd_input_aligned=%d", h264Decoder.simdAligned());
#else
        throw std::runtime_error("Native H264 decoder is not linked on this target");
#endif
      } else {
        // RGB565 never instantiates a JPEG decoder or a second pixel buffer.
        if (!decoder && jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK)
          throw std::runtime_error("MJPEG SIMD decoder allocation failed");
        jpeg_dec_io_t io{};
        io.inbuf = packet->bytes.data();
        io.inbuf_len = int(packet->bytes.size());
        jpeg_dec_header_info_t header{};
        if (jpeg_dec_parse_header(decoder, &io, &header) != JPEG_ERR_OK ||
            !header.width || !header.height || header.width > 512 || header.height > 512)
          throw std::runtime_error("MJPEG rejected JPEG dimensions/header");
        int outputBytes = 0;
        if (jpeg_dec_get_outbuf_len(decoder, &outputBytes) != JPEG_ERR_OK ||
            outputBytes != int(size_t(header.width) * header.height * sizeof(uint16_t)))
          throw std::runtime_error("MJPEG unexpected decoder output size");
        frame = frames.acquire(header.width, header.height);
        if (!frame) continue;
        io.outbuf = reinterpret_cast<uint8_t*>(frame->rgb565.data());
        if (jpeg_dec_process(decoder, &io) != JPEG_ERR_OK || io.out_size != outputBytes)
          throw std::runtime_error("MJPEG JPEG decode failed");
        frame->panelEndian = bool(GEA_EMBEDDED_PIXEL_PANEL_ENDIAN);
        frame->timestampMs = packet->timestampMs;
      }
      if (!running) break;
      media::publish_video_frame(track, std::move(frame));
      ++decoded;
      totalDecode += esp_timer_get_time() - started;
#if CONFIG_FREERTOS_USE_TRACE_FACILITY && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
      TaskStatus_t cpuAfter{};
      vTaskGetInfo(nullptr, &cpuAfter, pdFALSE, eRunning);
      workerCpuUs += cpuAfter.ulRunTimeCounter - cpuBefore.ulRunTimeCounter;
#endif
      const auto now = esp_timer_get_time();
      if (now - report >= 2000000) {
        ESP_LOGI("gea_mjpeg", "decoded=%u received=%u replaced=%u decode_us=%lld raw=%u h264=%u worker_cpu_us=%llu",
            decoded, received.load(), replaced.load(), (long long)(totalDecode / decoded), raw, h264,
            (unsigned long long)(workerCpuUs / decoded));
        if (h264) ESP_LOGI("gea_h264", "decode_us=%lld convert_us=%lld worker_cpu_us=%llu",
            (long long)(h264DecodeUs / h264), (long long)(h264ConvertUs / h264),
            (unsigned long long)(workerCpuUs / decoded));
        report = now;
      }
      vTaskDelay(1); // Keep raster and CPU1 idle work progressing under sustained video.
    }
  }

  template<bool Decode> static void worker(void* argument) {
    auto* owner = static_cast<std::shared_ptr<State>*>(argument);
    auto state = std::move(*owner);
    delete owner;
    try {
      if constexpr (Decode) state->decode();
      else state->receive();
    } catch (const std::exception& error) {
      if (state->running) ESP_LOGE("gea_mjpeg", "%s", error.what());
    }
    state->running = false;
    xSemaphoreGive(state->wake);
    state.reset();
    vTaskDeleteWithCaps(nullptr);
  }
#endif
};

MjpegStream::MjpegStream(const std::string& url) {
#ifdef GEA_HAS_MJPEG_STREAM
  if (!url.starts_with("http://") && !url.starts_with("https://"))
    throw std::invalid_argument("MJPEG requires an HTTP(S) URL");
  state_ = std::make_shared<State>();
  state_->url = url;
  state_->wake = xSemaphoreCreateBinary();
  if (!state_->wake) throw std::runtime_error("MJPEG wake allocation failed");
  // Each task holds its own source lifetime. Stop is immediate on the UI;
  // network timeouts bound cleanup without deleting a task inside HTTP/JPEG.
  for (const bool decode : {false, true}) {
    auto* owner = new std::shared_ptr<State>(state_);
    const auto entry = decode ? State::worker<true> : State::worker<false>;
    // Drain TCP promptly so rendering cannot close its small receive window.
    // JPEG decoding shares the UI's background priority. Audio stays above
    // both, and the receive worker blocks when no network data is available.
    // Full-image RGB565 JPEG decoding is integer/SIMD work. The codec's
    // floating-point block-count helper is not used or linked on this path.
    // Let FreeRTOS use the spare cycles on either core: fixed CPU0 competes
    // with capture/Wi-Fi, while fixed CPU1 competes with UI/response receive.
    // Audio remains higher priority; reception and rendering stay independent.
    const BaseType_t decodeCore = configNUMBER_OF_CORES > 1 ? tskNO_AFFINITY : 0;
    if (xTaskCreatePinnedToCoreWithCaps(entry, decode ? "gea_mjpeg_dec" : "gea_mjpeg_rx",
        decode ? 64 * 1024 : 16 * 1024, owner, decode ? 3 : 4, nullptr, decode ? decodeCore : 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
      delete owner;
      stop();
      throw std::runtime_error("MJPEG task allocation failed");
    }
  }
#else
  (void)url;
  throw std::runtime_error("MJPEG native streaming requires an ESP JPEG decoder");
#endif
}
MjpegStream::~MjpegStream() { stop(); }
MediaStream MjpegStream::stream() const { return state_ ? state_->source : MediaStream{}; }
void MjpegStream::stop() {
  if (!state_) return;
  state_->running = false;
#ifdef GEA_HAS_MJPEG_STREAM
  if (state_->wake) xSemaphoreGive(state_->wake);
#endif
}
} // namespace gea::host::video
