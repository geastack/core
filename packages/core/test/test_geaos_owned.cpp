// SPDX-License-Identifier: Apache-2.0
// OwnedSet (geaos_owned.h): the fixed-capacity table a host records an app's
// heap blocks, timers and files in, checked against std::unordered_map over
// millions of random operations, at full load and across the wrap-around.
#include "geaos_owned.h"

#include <cstdio>
#include <random>
#include <unordered_map>
#include <vector>

using gea::geaos::OwnedSet;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                    \
	do {                                                                               \
		g_checks++;                                                                    \
		if (!(cond)) {                                                                 \
			g_failures++;                                                              \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                              \
	} while (0)

bool same(const OwnedSet &set, const std::unordered_map<std::uintptr_t, std::uint32_t> &model)
{
	if (set.count() != model.size()) return false;
	std::uint64_t total = 0;
	for (const auto &kv : model) {
		if (!set.contains(kv.first)) return false;
		total += kv.second;
	}
	std::size_t seen = 0;
	bool ok = true;
	set.forEach([&](std::uintptr_t key, std::uint32_t value) {
		seen++;
		auto it = model.find(key);
		if (it == model.end() || it->second != value) ok = false;
	});
	return ok && seen == model.size() && set.total() == total;
}

void testBasics()
{
	OwnedSet empty;
	CHECK(!empty.ready());
	CHECK(!empty.add(1, 1));
	CHECK(!empty.remove(1));
	CHECK(!empty.contains(1));
	std::vector<OwnedSet::Slot> storage(100);  // rounds down to 64
	OwnedSet set;
	set.init(storage.data(), storage.size());
	CHECK(set.capacity() == 64);
	CHECK(!set.add(0, 5));  // key 0 is "empty"
	CHECK(set.add(0x1000, 16));
	CHECK(set.add(0x1000, 32));  // update, not a second member
	CHECK(set.count() == 1 && set.total() == 32);
	std::uint32_t value = 0;
	CHECK(set.remove(0x1000, &value) && value == 32);
	CHECK(!set.remove(0x1000));
	CHECK(set.count() == 0 && set.total() == 0);
	// Fill to the 7/8 limit: the next new key overflows, updates still work.
	for (std::uintptr_t k = 1; k <= 56; k++) CHECK(set.add(k * 8, 1));
	CHECK(!set.add(57 * 8, 1));
	CHECK(set.overflowed());
	CHECK(set.add(8, 2));
	CHECK(set.count() == 56);
	std::uintptr_t key;
	int taken = 0;
	while (set.takeAny(key, value)) taken++;
	CHECK(taken == 56 && set.count() == 0 && set.total() == 0);
	set.clear();
	CHECK(!set.overflowed());
}

// Keys that all hash near the end of the table, so probe runs wrap past the
// last slot and deletions must shift across the boundary.
void testWrapAround()
{
	std::vector<OwnedSet::Slot> storage(16);
	OwnedSet set;
	set.init(storage.data(), storage.size());
	std::unordered_map<std::uintptr_t, std::uint32_t> model;
	std::mt19937 rng(7);
	for (int round = 0; round < 200000; round++) {
		const std::uintptr_t key = 1 + rng() % 40;
		if (rng() % 2) {
			if (set.add(key, key)) model[key] = static_cast<std::uint32_t>(key);
			else CHECK(model.size() >= 14 && !model.count(key));
		} else {
			CHECK(set.remove(key) == (model.erase(key) == 1));
		}
		if (round % 997 == 0) CHECK(same(set, model));
	}
	CHECK(same(set, model));
}

// Heap-like keys (16-byte aligned, clustered) at high load, millions of ops.
void testRandomHeap()
{
	std::vector<OwnedSet::Slot> storage(4096);
	OwnedSet set;
	set.init(storage.data(), storage.size());
	std::unordered_map<std::uintptr_t, std::uint32_t> model;
	std::vector<std::uintptr_t> live;
	std::mt19937_64 rng(42);
	const std::uintptr_t base = 0x48000000;
	for (int i = 0; i < 3000000; i++) {
		const bool grow = live.size() < 3000 && (live.empty() || rng() % 100 < 52);
		if (grow) {
			const std::uintptr_t key = base + (rng() % (1 << 20)) * 16;
			const std::uint32_t size = static_cast<std::uint32_t>(rng() % 4096 + 1);
			const bool fresh = !model.count(key);
			CHECK(set.add(key, size));
			model[key] = size;
			if (fresh) live.push_back(key);
		} else {
			const std::size_t at = rng() % live.size();
			const std::uintptr_t key = live[at];
			live[at] = live.back();
			live.pop_back();
			std::uint32_t size = 0;
			CHECK(set.remove(key, &size) && size == model[key]);
			model.erase(key);
		}
		if (i % 100003 == 0) CHECK(same(set, model));
	}
	CHECK(same(set, model));
	CHECK(!set.overflowed());
}

}  // namespace

int main()
{
	testBasics();
	testWrapAround();
	testRandomHeap();
	if (g_failures) {
		std::fprintf(stderr, "FAIL geaos owned set: %d of %d checks failed\n", g_failures, g_checks);
		return 1;
	}
	std::printf("PASS geaos owned set: %d checks\n", g_checks);
	return 0;
}
