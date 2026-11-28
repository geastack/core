// SPDX-License-Identifier: Apache-2.0
// What a loaded app owns, so the host can take it back when the app exits
// without the app's cooperation: heap blocks, timers, open files.
//
// OwnedSet is a fixed-capacity hash set of handles (a pointer, a file
// descriptor) with a 32-bit value each (a block's size). It never allocates
// after init(): a host updates it from inside its allocator's hooks, where
// allocating would recurse. Linear probing with backward-shift deletion, so
// a long-running app that allocates and frees millions of times never fills
// the table with tombstones. Not thread-safe: the host holds its own lock.
#pragma once

#include <cstddef>
#include <cstdint>

namespace gea::geaos {

class OwnedSet {
public:
	struct Slot {
		std::uintptr_t key;  // 0 = empty
		std::uint32_t value;
	};

	// `capacity` is rounded down to a power of two; the table refuses new
	// keys at 7/8 full (overflowed() then says the record is incomplete).
	void init(Slot *storage, std::size_t capacity)
	{
		std::size_t pow2 = 1;
		while (pow2 * 2 <= capacity) pow2 *= 2;
		slots_ = storage;
		mask_ = storage && capacity ? pow2 - 1 : 0;
		limit_ = storage && capacity ? pow2 - pow2 / 8 : 0;
		clear();
	}

	void clear()
	{
		if (slots_)
			for (std::size_t i = 0; i <= mask_; i++) slots_[i] = Slot{0, 0};
		count_ = 0;
		total_ = 0;
		overflowed_ = false;
	}

	bool ready() const { return slots_ != nullptr; }

	// Adds or updates `key`. False (and overflowed()) when full; key 0 is
	// never stored.
	bool add(std::uintptr_t key, std::uint32_t value)
	{
		if (!slots_ || key == 0) return false;
		std::size_t i = indexOf(key);
		while (slots_[i].key != 0) {
			if (slots_[i].key == key) {
				total_ -= slots_[i].value;
				slots_[i].value = value;
				total_ += value;
				return true;
			}
			i = (i + 1) & mask_;
		}
		if (count_ >= limit_) {
			overflowed_ = true;
			return false;
		}
		slots_[i] = Slot{key, value};
		count_++;
		total_ += value;
		return true;
	}

	bool contains(std::uintptr_t key) const { return find(key) != npos; }

	// Removes `key`; false if it was not there.
	bool remove(std::uintptr_t key, std::uint32_t *value = nullptr)
	{
		std::size_t i = find(key);
		if (i == npos) return false;
		if (value) *value = slots_[i].value;
		total_ -= slots_[i].value;
		count_--;
		// Backward shift: pull later members of the probe run into the hole
		// so every remaining key stays reachable from its home slot.
		std::size_t hole = i;
		std::size_t j = i;
		for (;;) {
			j = (j + 1) & mask_;
			if (slots_[j].key == 0) break;
			const std::size_t home = indexOf(slots_[j].key);
			// j's key may move to `hole` only if its home is not in (hole, j].
			const bool between = hole <= j ? (home > hole && home <= j) : (home > hole || home <= j);
			if (between) continue;
			slots_[hole] = slots_[j];
			hole = j;
		}
		slots_[hole] = Slot{0, 0};
		return true;
	}

	std::size_t count() const { return count_; }
	std::uint64_t total() const { return total_; }
	bool overflowed() const { return overflowed_; }
	std::size_t capacity() const { return slots_ ? mask_ + 1 : 0; }

	// Calls f(key, value) for every member. f must not modify the set; take
	// the keys out first (drain) to act on them.
	template <class F>
	void forEach(F &&f) const
	{
		if (!slots_) return;
		for (std::size_t i = 0; i <= mask_; i++)
			if (slots_[i].key) f(slots_[i].key, slots_[i].value);
	}

	// Removes and returns one member (false when empty): the way to release
	// everything while the release itself may touch the set.
	bool takeAny(std::uintptr_t &key, std::uint32_t &value)
	{
		if (!slots_ || count_ == 0) return false;
		for (std::size_t i = 0; i <= mask_; i++) {
			if (!slots_[i].key) continue;
			key = slots_[i].key;
			value = slots_[i].value;
			remove(key);
			return true;
		}
		return false;
	}

private:
	static constexpr std::size_t npos = ~static_cast<std::size_t>(0);

	std::size_t indexOf(std::uintptr_t key) const
	{
		// Heap pointers share low zero bits and close high bits: mix them.
		std::uint64_t x = static_cast<std::uint64_t>(key);
		x ^= x >> 33;
		x *= 0xff51afd7ed558ccdULL;
		x ^= x >> 33;
		return static_cast<std::size_t>(x) & mask_;
	}

	std::size_t find(std::uintptr_t key) const
	{
		if (!slots_ || key == 0) return npos;
		std::size_t i = indexOf(key);
		while (slots_[i].key != 0) {
			if (slots_[i].key == key) return i;
			i = (i + 1) & mask_;
		}
		return npos;
	}

	Slot *slots_ = nullptr;
	std::size_t mask_ = 0;
	std::size_t limit_ = 0;
	std::size_t count_ = 0;
	std::uint64_t total_ = 0;
	bool overflowed_ = false;
};

}  // namespace gea::geaos
