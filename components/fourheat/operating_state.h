#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace esphome::fourheat {

inline constexpr char POWER_STATE_QUERY[] = "I30001";
inline constexpr char POWER_STATE_DATAPOINT[] = "J30001";

enum class OperatingState : int {
  UNKNOWN = -1,
  OFF = 0,
  CHECK_UP = 1,
  IGNITION = 2,
  STABILIZATION = 3,
  IGNITION_PHASE = 4,
  NORMAL = 5,
  MODULATION = 6,
  SHUTDOWN = 7,
  SAFETY = 8,
  LOCKOUT = 9,
  IGNITION_RECOVERY = 10,
  STANDBY = 11,
  IGNITION_30 = 30,
  IGNITION_31 = 31,
  IGNITION_32 = 32,
  IGNITION_33 = 33,
  IGNITION_34 = 34,
};

std::optional<int> parse_operating_state_code(const std::vector<uint8_t> &data);
OperatingState decode_operating_state(const std::vector<uint8_t> &data);

bool can_start(OperatingState state);
bool can_stop(OperatingState state);
bool is_alarm(OperatingState state);
std::optional<bool> power_direction(OperatingState state);

}  // namespace esphome::fourheat
