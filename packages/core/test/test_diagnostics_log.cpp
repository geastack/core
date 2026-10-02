// Exercise the real diagnostic ring under wraparound and concurrent producers.
#include "services/diagnostics.h"
#include "services/diagnostics_platform.h"
#include "diagnostics_internal.h"
#include "memory.h"

#include <array>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace gea::framework::memory {
void *Allocator::allocatePreferSpiram(std::size_t size, std::size_t) { return std::malloc(size); }
void *Allocator::reallocatePreferSpiram(void *pointer, std::size_t size) { return std::realloc(pointer, size); }
void Allocator::free(void *pointer) noexcept { std::free(pointer); }
}

namespace gea::framework::services {
DiagnosticsPlatform &diagnosticsPlatform() { std::abort(); }
namespace diagnostics_internal {
void startServer() { std::abort(); }
}
}

int main()
{
  using gea::framework::services::DiagnosticsServer;
  namespace logs = gea::framework::services::diagnostics_internal;
  constexpr std::size_t capacity = 8192;
  assert(DiagnosticsServer::begin());

  std::string input(capacity * 3 + 17, ' ');
  for (std::size_t i = 0; i < input.size(); ++i) input[i] = char('A' + i % 26);
  logs::writeLog(input.data(), input.size());
  assert(logs::latestLogTotal() == input.size());
  assert(logs::oldestLogTotal() == input.size() - capacity);
  assert(DiagnosticsServer::begin());
  assert(logs::latestLogTotal() == input.size()); // Startup is idempotent.

  std::size_t cursor = 0;
  std::string replay;
  char piece[31];
  while (const int count = logs::copyLogSince(&cursor, piece, sizeof(piece))) replay.append(piece, count);
  assert(replay == input.substr(input.size() - capacity));
  assert(cursor == logs::latestLogTotal());

  // A normal formatted line beyond the short stack buffer remains complete.
  const std::string longLine(1500, 'x');
  const auto beforeLine = logs::latestLogTotal();
  DiagnosticsServer::print("%s\n", longLine.c_str());
  cursor = beforeLine;
  replay.clear();
  while (const int count = logs::copyLogSince(&cursor, piece, sizeof(piece))) replay.append(piece, count);
  assert(replay == longLine + "\n");

  // Each producer writes one record at a time. No record may contain bytes
  // from another producer, including writes that wrap the physical ring.
  // Align the logical stream so the latest ringful contains whole records.
  const auto padding = (256 - logs::latestLogTotal() % 256) % 256;
  const std::string pad(padding, '-');
  logs::writeLog(pad.data(), pad.size());
  const auto beforeThreads = logs::latestLogTotal();
  std::vector<std::thread> producers;
  for (unsigned producer = 0; producer < 4; ++producer) {
    producers.emplace_back([producer] {
      const std::string record(256, char('a' + producer));
      for (unsigned i = 0; i < 1000; ++i) logs::writeLog(record.data(), record.size());
    });
  }
  for (auto &producer : producers) producer.join();
  assert(logs::latestLogTotal() == beforeThreads + 4 * 1000 * 256);
  cursor = logs::oldestLogTotal();
  std::array<char, capacity> tail{};
  assert(logs::copyLogSince(&cursor, tail.data(), tail.size()) == int(capacity));
  for (std::size_t record = 0; record < capacity; record += 256) {
    assert(tail[record] >= 'a' && tail[record] <= 'd');
    for (unsigned i = 1; i < 256; ++i) assert(tail[record + i] == tail[record]);
  }
  assert(logs::copyLogSince(&cursor, tail.data(), tail.size()) == 0);
  std::fprintf(stderr, "PASS diagnostic ring: wrap, oversized write, long formatting, idempotent startup, concurrent producers\n");
}
