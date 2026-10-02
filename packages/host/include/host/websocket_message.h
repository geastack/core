#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace gea::host::websocket {

// ESP-IDF splits large frames into DATA events; WebSocket continuation frames
// may split one message further. JavaScript receives one complete text message.
class TextMessageAssembler {
public:
  enum class Result { Pending, Complete, Invalid };

  Result append(int opcode, bool fin, std::size_t offset, std::size_t frameBytes,
                std::string_view bytes) {
    if (opcode >= 8) return Result::Pending; // Interleaved ping/pong/close.
    if (offset == 0) {
      if (expectedOffset_ != frameBytes_) return invalid();
      if (opcode == 1) {
        if (textOpen_) return invalid();
        textOpen_ = true;
        message_.clear();
      } else if (opcode == 2) {
        if (textOpen_) return invalid();
      } else if (opcode != 0) {
        return invalid();
      }
      expectedOffset_ = 0;
      frameBytes_ = frameBytes;
      opcode_ = opcode;
    }
    if (offset != expectedOffset_ || frameBytes != frameBytes_ || opcode != opcode_ ||
        offset > frameBytes || bytes.size() > frameBytes - offset) return invalid();
    expectedOffset_ += bytes.size();
    if (!textOpen_) return Result::Pending; // Binary messages are not exposed here.
    if (bytes.size() > kMaxMessageBytes - message_.size()) return invalid();
    message_.append(bytes);
    if (fin && expectedOffset_ == frameBytes_) {
      textOpen_ = false;
      return Result::Complete;
    }
    return Result::Pending;
  }

  std::string take() { return std::exchange(message_, {}); }

private:
  Result invalid() {
    message_.clear();
    textOpen_ = false;
    expectedOffset_ = frameBytes_ = 0;
    opcode_ = -1;
    return Result::Invalid;
  }

  static constexpr std::size_t kMaxMessageBytes = 1024 * 1024;
  std::string message_;
  std::size_t expectedOffset_ = 0;
  std::size_t frameBytes_ = 0;
  int opcode_ = -1;
  bool textOpen_ = false;
};

} // namespace gea::host::websocket
