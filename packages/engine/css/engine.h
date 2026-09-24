// SPDX-License-Identifier: Apache-2.0
#pragma once
//
// gea::css — the animation engine/driver. Holds the active animations, advances
// them each frame, and applies the interpolated value to the target node via
// ui::Style. Tick from the frame loop. Header-only (inline) so it links into any
// TU that uses it without a build source-list change. The singleton lives in an
// inline function-local static (one instance across all TUs). See
// docs/geaos-grand-vision.md (M1).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <utility>
#include <vector>

#include "../ui/style.h"
#include "animation.h"

#ifndef GEA_CSS_ANIMATION_INLINE_RUNNING
#define GEA_CSS_ANIMATION_INLINE_RUNNING 8
#endif

namespace gea::css {

class AnimationEngine {
public:
  static AnimationEngine &instance()
  {
    static AnimationEngine engine;
    return engine;
  }

  // Register an animation; returns a handle (>0) for later cancel.
  int start(Animation a, uint32_t nowMs)
  {
    const int handle = nextHandle_++;
    running_.push_back(Running{std::move(a), nowMs, handle});
    return handle;
  }
  void cancel(int handle)
  {
    running_.eraseIf([&](const Running &r) { return r.handle == handle; });
  }
  void cancelNode(int nodeId)  // cancel all animations targeting a node
  {
    running_.eraseIf([&](const Running &r) { return r.anim.nodeId == nodeId; });
  }
  void clear() { running_.clear(); }

  bool active() const { return !running_.empty(); }
  std::size_t count() const { return running_.size(); }

  // Advance all animations to `nowMs`, applying values and dropping finished
  // ones (forwards-fill holds the final value before removal). Call once/frame.
  void tick(uint32_t nowMs)
  {
    for (std::size_t i = 0; i < running_.size();) {
      const Running &first = running_[i];
      const double elapsed = static_cast<double>(static_cast<int32_t>(nowMs - first.startMs));
      const Progress pr = computeProgress(first.anim, elapsed);
      std::size_t groupEnd = i + 1;
      while (groupEnd < running_.size() && sameTimeline(first, running_[groupEnd])) ++groupEnd;
      if (pr.active) {
        for (std::size_t j = i; j < groupEnd; ++j) {
          const auto &a = running_[j].anim;
          if (!a.rotationAxes.empty())
            applyRotationSample(a, pr.p, [&](Property property, int value) {
              gea::embedded::ui::applyAnimatedStyleValue(a.nodeId, property, value);
            });
          else applyValue(a, sampleTrack(a, pr.p));
        }
      }
      if (pr.done)
        running_.erase(i, groupEnd);
      else
        i = groupEnd;
    }
  }

private:
  AnimationEngine() = default;

  struct Running {
    Animation anim;
    uint32_t startMs;
    int handle;
  };

  class RunningList {
  public:
    bool empty() const { return size() == 0; }
    std::size_t size() const { return spilled_ ? spill_.size() : count_; }

    Running &operator[](std::size_t index) { return spilled_ ? spill_[index] : inline_[index]; }
    const Running &operator[](std::size_t index) const { return spilled_ ? spill_[index] : inline_[index]; }

    void push_back(Running value)
    {
      if (spilled_) {
        spill_.push_back(std::move(value));
        return;
      }
      if (count_ < kInlineCapacity) {
        inline_[count_++] = std::move(value);
        return;
      }
      spillToVector(kInlineCapacity + 1);
      spill_.push_back(std::move(value));
    }

    void erase(std::size_t first, std::size_t last)
    {
      if (first >= last) return;
      if (spilled_) {
        spill_.erase(spill_.begin() + static_cast<std::ptrdiff_t>(first),
                     spill_.begin() + static_cast<std::ptrdiff_t>(last));
        if (spill_.size() <= kInlineCapacity) moveSpillBackInline();
        return;
      }
      const std::size_t removed = last - first;
      for (std::size_t read = last; read < count_; ++read) {
        inline_[read - removed] = std::move(inline_[read]);
      }
      const std::size_t nextCount = count_ - removed;
      for (std::size_t i = nextCount; i < count_; ++i) inline_[i] = Running{};
      count_ = nextCount;
    }

    template <typename Predicate>
    void eraseIf(Predicate pred)
    {
      if (spilled_) {
        spill_.erase(std::remove_if(spill_.begin(), spill_.end(), pred), spill_.end());
        if (spill_.size() <= kInlineCapacity) moveSpillBackInline();
        return;
      }
      std::size_t write = 0;
      for (std::size_t read = 0; read < count_; ++read) {
        if (pred(inline_[read])) continue;
        if (write != read) inline_[write] = std::move(inline_[read]);
        ++write;
      }
      for (std::size_t i = write; i < count_; ++i) inline_[i] = Running{};
      count_ = write;
    }

    void clear()
    {
      if (spilled_) {
        std::vector<Running>().swap(spill_);
        spilled_ = false;
        count_ = 0;
        return;
      }
      for (std::size_t i = 0; i < count_; ++i) inline_[i] = Running{};
      count_ = 0;
    }

  private:
    static constexpr std::size_t kInlineCapacity =
        static_cast<std::size_t>(GEA_CSS_ANIMATION_INLINE_RUNNING);

    void spillToVector(std::size_t reserveCount)
    {
      if (spilled_) {
        spill_.reserve(reserveCount);
        return;
      }
      spill_.reserve(reserveCount);
      for (std::size_t i = 0; i < count_; ++i) spill_.push_back(std::move(inline_[i]));
      for (std::size_t i = 0; i < count_; ++i) inline_[i] = Running{};
      count_ = 0;
      spilled_ = true;
    }

    void moveSpillBackInline()
    {
      if (!spilled_ || spill_.size() > kInlineCapacity) return;
      count_ = spill_.size();
      for (std::size_t i = 0; i < count_; ++i) inline_[i] = std::move(spill_[i]);
      std::vector<Running>().swap(spill_);
      spilled_ = false;
    }

    std::array<Running, kInlineCapacity> inline_{};
    std::vector<Running> spill_;
    std::size_t count_ = 0;
    bool spilled_ = false;
  };

  static bool sameTimeline(const Running &a, const Running &b)
  {
    return a.startMs == b.startMs && a.anim.durationMs == b.anim.durationMs &&
           a.anim.delayMs == b.anim.delayMs && a.anim.iterations == b.anim.iterations &&
           a.anim.direction == b.anim.direction && a.anim.fill == b.anim.fill;
  }

  static void applyValue(const Animation &a, double value)
  {
    using gea::embedded::ui::Property;
    using gea::embedded::ui::applyAnimatedStyleValue;
    switch (a.kind) {
    case ValueKind::Angle:
      applyAnimatedStyleValue(a.nodeId, a.property, static_cast<int>(std::llround(value * 10.0)));
      return;
    case ValueKind::Color: {
      const int rgb565 = static_cast<int>(std::llround(value));
      applyAnimatedStyleValue(a.nodeId, a.property, rgb565);
      if (a.property == Property::BackgroundColor)
        applyAnimatedStyleValue(a.nodeId, Property::HasBackground, 1);
      else if (a.property == Property::ActiveBackgroundColor)
        applyAnimatedStyleValue(a.nodeId, Property::HasActiveBackground, 1);
      return;
    }
    case ValueKind::Scalar:
      applyAnimatedStyleValue(a.nodeId, a.property, static_cast<int>(std::llround(value)));
      return;
    }
  }

  RunningList running_;
  int nextHandle_ = 1;
};

}  // namespace gea::css
