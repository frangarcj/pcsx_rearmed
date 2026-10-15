/* Minimal deterministic replacement for MAME's device/timer plumbing.
 * The ATA command bodies in the adjacent imported files remain pinned MAME code.
 */
#ifndef PCSX_ATA_MAME_COMPAT_H
#define PCSX_ATA_MAME_COMPAT_H

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

using offs_t = uint32_t;

class attotime {
public:
  constexpr explicit attotime(int64_t nanoseconds = 0) : ns(nanoseconds) {}
  static attotime from_nsec(int64_t value) { return attotime(value); }
  static attotime from_usec(int64_t value) { return attotime(value * 1000); }
  static attotime from_msec(int64_t value) { return attotime(value * 1000000); }
  static attotime from_hz(int64_t value) {
    return attotime(value > 0 ? 1000000000 / value : 0);
  }
  static const attotime zero;
  static const attotime never;
  friend attotime operator*(attotime time, int64_t factor) {
    return attotime(time.ns * factor);
  }
  friend attotime operator+(attotime left, attotime right) {
    if (left.ns == std::numeric_limits<int64_t>::max() ||
        right.ns == std::numeric_limits<int64_t>::max() ||
        right.ns > std::numeric_limits<int64_t>::max() - left.ns)
      return never;
    return attotime(left.ns + right.ns);
  }
  friend attotime operator/(attotime time, int64_t divisor) {
    return attotime(divisor ? time.ns / divisor : 0);
  }
  friend bool operator>(attotime left, attotime right) {
    return left.ns > right.ns;
  }
  int64_t as_nanoseconds() const { return ns; }

private:
  int64_t ns;
};

inline const attotime attotime::zero{0};
inline const attotime attotime::never{std::numeric_limits<int64_t>::max()};

class device_t;
using timer_expired_delegate = std::function<void(int32_t)>;
class machine_stub {
public:
  bool side_effects_disabled() const { return false; }
  const char *describe_context() const { return "standalone ATA HLE"; }
};
class emu_timer {
public:
  emu_timer(device_t *device, std::function<void(int32_t)> callback)
      : owner(device), invoke(std::move(callback)) {}
  void adjust(attotime duration, int32_t param = 0);
  void enable(bool enabled);
  int32_t param() const { return callback_param; }
  attotime elapsed() const;

private:
  friend class device_t;
  device_t *owner;
  std::function<void(int32_t)> invoke;
  int64_t due_ns = std::numeric_limits<int64_t>::max();
  int64_t started_ns = 0;
  int32_t callback_param = 0;
  bool active = false;
};

class device_t {
public:
  device_t() = default;
  virtual ~device_t() = default;
  virtual void device_start() {}
  virtual void device_reset() {}
  machine_stub &machine() { return machine_context; }

  template <typename Owner>
  emu_timer *timer_alloc(void (Owner::*callback)(int32_t), Owner *owner) {
    return timer_alloc([owner, callback](int32_t p) { (owner->*callback)(p); });
  }

  emu_timer *timer_alloc(timer_expired_delegate callback = {}) {
    auto timer = std::make_unique<emu_timer>(this, std::move(callback));
    emu_timer *result = timer.get();
    result->started_ns = now_ns;
    timers.push_back(std::move(timer));
    return result;
  }

  bool run_until(uint64_t monotonic_ns) {
    if (monotonic_ns > uint64_t(std::numeric_limits<int64_t>::max()) ||
        int64_t(monotonic_ns) < now_ns)
      return false;
    for (;;) {
      emu_timer *next = nullptr;
      for (const auto &timer : timers) {
        if (timer->active && (!next || timer->due_ns < next->due_ns))
          next = timer.get();
      }
      if (!next || uint64_t(next->due_ns) > monotonic_ns)
        break;
      now_ns = next->due_ns;
      next->active = false;
      if (next->invoke)
        next->invoke(next->callback_param);
    }
    now_ns = int64_t(monotonic_ns);
    return true;
  }
  uint64_t now() const { return uint64_t(now_ns); }
  uint64_t next_event_ns() const {
    uint64_t next = std::numeric_limits<uint64_t>::max();
    for (const auto &timer : timers)
      if (timer->active && uint64_t(timer->due_ns) < next)
        next = uint64_t(timer->due_ns);
    return next;
  }

private:
  friend class emu_timer;
  int64_t now_ns = 0;
  machine_stub machine_context;
  std::vector<std::unique_ptr<emu_timer>> timers;
};

inline void emu_timer::adjust(attotime duration, int32_t param) {
  callback_param = param;
  started_ns = owner->now_ns;
  const int64_t delay = duration.as_nanoseconds();
  if (delay == std::numeric_limits<int64_t>::max()) {
    active = false;
    due_ns = delay;
  } else {
    due_ns = delay > std::numeric_limits<int64_t>::max() - owner->now_ns
                 ? std::numeric_limits<int64_t>::max()
                 : owner->now_ns + delay;
    active = true;
  }
}
inline void emu_timer::enable(bool enabled) { active = enabled; }
inline attotime emu_timer::elapsed() const {
  return attotime(owner->now_ns - started_ns);
}

#define ATTR_COLD
#define TIMER_CALLBACK_MEMBER(name) void name([[maybe_unused]] int32_t param)
#define FUNC(name) &name
#define NAME(name) name
#define save_item(...) ((void)0)
#define LOG(...) ((void)0)
#define LOGMASKED(...) ((void)0)
#define osd_printf_debug(...) ((void)0)
#define ASSERT_LINE 1
#define CLEAR_LINE 0

inline void put_u16le(void *dst, uint16_t value) {
  auto *bytes = static_cast<uint8_t *>(dst);
  bytes[0] = uint8_t(value);
  bytes[1] = uint8_t(value >> 8);
}
inline void put_u32be(void *dst, uint32_t value) {
  auto *bytes = static_cast<uint8_t *>(dst);
  bytes[0] = uint8_t(value >> 24);
  bytes[1] = uint8_t(value >> 16);
  bytes[2] = uint8_t(value >> 8);
  bytes[3] = uint8_t(value);
}
inline uint16_t get_u16le(const void *src) {
  const auto *bytes = static_cast<const uint8_t *>(src);
  return uint16_t(bytes[0]) | (uint16_t(bytes[1]) << 8);
}

#endif
