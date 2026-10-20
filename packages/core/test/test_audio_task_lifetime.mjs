import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import { mkdirSync, readFileSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const source = readFileSync(new URL('../../host/audio_runtime.cpp', import.meta.url), 'utf8')
const build = process.env.GEA_TEST_BUILD_DIR || fileURLToPath(new URL('../../../build/', import.meta.url))
mkdirSync(build, { recursive: true })
// Windows has no `c++`, and spawn finds an executable only by its .exe name.
const windows = process.platform === 'win32'
const binary = path.join(build, `audio-task-lifetime-test${windows ? '.exe' : ''}`)

function method(signature) {
  const start = source.indexOf(signature)
  assert.notEqual(start, -1, `missing production method ${signature}`)
  const brace = source.indexOf('{', start)
  let depth = 1, end = brace + 1
  while (depth && end < source.length) {
    if (source[end] === '{') depth++
    else if (source[end] === '}') depth--
    end++
  }
  assert.equal(depth, 0)
  return source.slice(start, end)
}
const membersStart = source.indexOf('  static constexpr const char *kTag = "gea_audio";')
const membersEnd = source.indexOf('\n};', membersStart)
assert.ok(membersStart >= 0 && membersEnd > membersStart)

// Execute the real creation/failure paths and member declarations. Platform
// fakes never launch the infinite audio worker, but enforce its allocation caps,
// lifetime, task priority, and the one-TCB/one-task rule under concurrent starts.
for (const timing of [0, 1]) {
  const program = `
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#define GEA_AUDIO_DEBUG_TIMING ${timing}
#define GEA_AUDIO_DEBUG_TIMING_THRESHOLD_US 500
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
using StackType_t = std::uint8_t;
using QueueHandle_t = void *;
using TaskHandle_t = void *;
using UBaseType_t = unsigned;
struct StaticTask_t { char bytes[128]; };
struct StaticQueue_t { char bytes[128]; };
constexpr int kTaskStackBytes = 8192, kQueueDepth = 16, kMaxOscillators = 8, kMaxActiveTones = 12;
constexpr UBaseType_t kTaskPriority = 24;
constexpr unsigned MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_8BIT = 2;
struct Tone { int value = 0; };
struct OscillatorState { int value = 0; };
struct ActiveTone { int value = 0; };
namespace audio {
struct PcmStreamMixer {};
struct OutputDriver {
  static inline int level = 0;
  static int volume() { return level; }
  static void setVolume(int value) { level = value; }
};
}
std::atomic<int> allocations{0}, frees{0}, creates{0}, queues{0};
std::atomic<bool> failAllocation{false}, failCreation{false}, failQueue{false};
std::mutex heapMutex;
std::map<void *, std::size_t> live;
std::int64_t esp_timer_get_time() { return 1234; }
void *heap_caps_malloc(std::size_t size, unsigned caps) {
  assert(size == kTaskStackBytes);
  assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  ++allocations;
  if (failAllocation.exchange(false)) return nullptr;
  void *p = std::malloc(size);
  assert(p);
  { std::lock_guard<std::mutex> guard(heapMutex); live.emplace(p, size); }
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  return p;
}
void heap_caps_free(void *p) {
  assert(p);
  { std::lock_guard<std::mutex> guard(heapMutex); assert(live.erase(p) == 1); }
  ++frees;
  std::free(p);
}
QueueHandle_t xQueueCreateStatic(int count, std::size_t size, std::uint8_t *, StaticQueue_t *storage) {
  assert(count == kQueueDepth && size == sizeof(Tone));
  ++queues;
  return failQueue.exchange(false) ? nullptr : storage;
}
TaskHandle_t xTaskCreateStatic(void (*)(void *), const char *name, int size, void *,
                              UBaseType_t priority, StackType_t *stack, StaticTask_t *storage) {
  assert(std::strcmp(name, "gea_audio") == 0 && size == kTaskStackBytes && priority == kTaskPriority);
  { std::lock_guard<std::mutex> guard(heapMutex); assert(live.count(stack) == 1); }
  ++creates;
  return failCreation.exchange(false) ? nullptr : storage;
}
class AudioEngine {
public:
  static void audioTaskEntry(void *) {}
  ${method('  int volume() const')}
  ${method('  void setVolume(int volumePercent)')}
  ${method('  bool ensureTask()')}
  ${source.slice(membersStart, membersEnd)}
};
int main() {
  AudioEngine controls;
  controls.setVolume(71);
  assert(controls.volume() == 71);
  assert(allocations == 0 && queues == 0 && creates == 0);

  failQueue = true;
  assert(!controls.ensureTask());
  assert(allocations == 0 && queues == 1);
  failAllocation = true;
  assert(!controls.ensureTask());
  assert(allocations == 1 && creates == 0 && frees == 0 && queues == 2);
  failCreation = true;
  assert(!controls.ensureTask());
  assert(allocations == 2 && creates == 1 && frees == 1);
  assert(controls.audioTaskStack_ == nullptr && controls.audioTask_ == nullptr);
  assert(controls.ensureTask());
  auto *stack = controls.audioTaskStack_;
  assert(stack && allocations == 3 && creates == 2 && frees == 1);
  for (int i = 0; i < 20; ++i) assert(controls.ensureTask());
  assert(controls.audioTaskStack_ == stack && allocations == 3 && creates == 2 && queues == 2);

  AudioEngine concurrent;
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::vector<std::thread> callers;
  for (int i = 0; i < 16; ++i) callers.emplace_back([&] {
    ++ready;
    while (!go.load()) std::this_thread::yield();
    assert(concurrent.ensureTask());
  });
  while (ready != 16) std::this_thread::yield();
  go = true;
  for (auto &caller : callers) caller.join();
  assert(allocations == 4 && creates == 3 && queues == 3 && frees == 1);
  assert(concurrent.audioTaskStack_ && concurrent.audioTaskStack_ != stack);

  // The fake workers never ran; release their retained stacks after all test
  // callers exit. Production workers retain these stacks for their lifetime.
  heap_caps_free(controls.audioTaskStack_);
  heap_caps_free(concurrent.audioTaskStack_);
  assert(live.empty());
}
`
  const compiled = spawnSync(process.env.CXX || (windows ? 'clang++' : 'c++'), [
    '-std=c++20', '-O1', '-g', '-pthread', '-fsanitize=address,undefined',
    '-x', 'c++', '-', '-o', binary,
  ], { input: program, encoding: 'utf8', env: { ...process.env, TMPDIR: build } })
  assert.ifError(compiled.error)
  assert.equal(compiled.status, 0, compiled.stderr)
  const ran = spawnSync(binary, [], { encoding: 'utf8', timeout: 15000 })
  assert.ifError(ran.error)
  assert.equal(ran.status, 0, ran.stderr)
}
console.log('Audio task lifetime: allocation-free controls, failure/retry and concurrent startup passed')
