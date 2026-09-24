// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace gea::embedded::ui {

// CSS Flexbox 2, 9.7: contiguous, nonempty lines minimizing squared unused
// space. Sizes include margins and are floored at zero only for balancing.
// Returns exclusive line ends. An oversized item occupies a line by itself.
inline std::vector<int> balancedFlexLineEnds(const std::vector<int> &sizes, int available, int gap, int minimumLines)
{
	const int n = static_cast<int>(sizes.size());
	if (!n) return {};
	std::vector<std::int64_t> prefix(n + 1);
	for (int i = 0; i < n; ++i) prefix[i + 1] = prefix[i] + std::max(0, sizes[i]) + gap;
	std::vector<int> furthest(n), required(n + 1);
	for (int i = n - 1; i >= 0; --i) {
		const auto bound = std::upper_bound(prefix.begin() + i + 1, prefix.end(), prefix[i] + available + gap);
		furthest[i] = std::max(i + 1, static_cast<int>(bound - prefix.begin()) - 1);
		required[i] = 1 + required[furthest[i]];
	}
	const int count = std::max(required[0], std::min(n, minimumLines));
	if (count == 1) return {n};
	std::vector<int> ends(count);
	if (count == n) {
		for (int i = 0; i < n; ++i) ends[i] = i + 1;
		return ends;
	}

	constexpr std::int64_t infinity = INT64_MAX / 4;
	std::vector<std::int64_t> previous(n + 1, infinity), current(n + 1, infinity);
	std::vector<int> choices((count + 1) * (n + 1), -1);
	previous[n] = 0;
	int previousFirst = n;
	for (int lines = 1; lines <= count; ++lines) {
		int first = 0;
		while (required[first] > lines) ++first;
		std::fill(current.begin(), current.end(), infinity);
		// Squared interval sums form a Monge cost matrix: optimal next breaks
		// are monotone. Divide-and-conquer computes a row in O(n log n), avoiding
		// a quadratic search per line. Suffix costs let ties favor the largest
		// FIRST break, then recursively apply the same preference to later lines.
		auto solve = [&](auto &&self, int lo, int hi, int optLo, int optHi) -> void {
			if (lo > hi) return;
			const int start = (lo + hi) / 2;
			const int begin = std::max({start + 1, previousFirst, optLo});
			const int end = std::min({furthest[start], n - lines + 1, optHi});
			int best = begin;
			for (int next = begin; next <= end; ++next) {
				const std::int64_t error = available - (prefix[next] - prefix[start] - gap);
				const std::int64_t cost = previous[next] + error * error;
				if (cost <= current[start]) { current[start] = cost; best = next; }
			}
			choices[lines * (n + 1) + start] = best;
			self(self, lo, start - 1, optLo, best);
			self(self, start + 1, hi, best, optHi);
		};
		solve(solve, first, n - lines, first + 1, n - lines + 1);
		previous.swap(current);
		previousFirst = first;
	}
	int start = 0;
	for (int line = 0; line < count; ++line) {
		start = choices[(count - line) * (n + 1) + start];
		ends[line] = start;
	}
	return ends;
}

} // namespace gea::embedded::ui
