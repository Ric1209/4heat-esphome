#include "operating_state.h"

#include <limits>

namespace esphome::fourheat {

std::optional<int> parse_operating_state_code(const std::vector<uint8_t> &data) {
  if (data.empty()) {
    return std::nullopt;
  }
  int code = 0;
  for (uint8_t byte : data) {
    if (byte < '0' || byte > '9' || code > (std::numeric_limits<int>::max() - (byte - '0')) / 10) {
      return std::nullopt;
    }
    code = code * 10 + (byte - '0');
  }
  return code;
}

OperatingState decode_operating_state(const std::vector<uint8_t> &data) {
  const auto state = static_cast<OperatingState>(parse_operating_state_code(data).value_or(-1));
  if ((state >= OperatingState::OFF && state <= OperatingState::STANDBY) ||
      (state >= OperatingState::IGNITION_30 && state <= OperatingState::IGNITION_34)) {
    return state;
  }
  return OperatingState::UNKNOWN;
}

bool can_start(OperatingState state) { return state == OperatingState::OFF; }

bool can_stop(OperatingState state) {
  return state == OperatingState::NORMAL || state == OperatingState::MODULATION;
}

bool is_alarm(OperatingState state) {
  return state == OperatingState::SAFETY || state == OperatingState::LOCKOUT;
}

std::optional<bool> power_direction(OperatingState state) {
  switch (state) {
    case OperatingState::OFF:
    case OperatingState::SHUTDOWN:
    case OperatingState::SAFETY:
    case OperatingState::LOCKOUT:
      return false;
    case OperatingState::CHECK_UP:
    case OperatingState::IGNITION:
    case OperatingState::STABILIZATION:
    case OperatingState::IGNITION_PHASE:
    case OperatingState::NORMAL:
    case OperatingState::MODULATION:
    case OperatingState::IGNITION_RECOVERY:
    case OperatingState::STANDBY:
    case OperatingState::IGNITION_30:
    case OperatingState::IGNITION_31:
    case OperatingState::IGNITION_32:
    case OperatingState::IGNITION_33:
    case OperatingState::IGNITION_34:
      return true;
    case OperatingState::UNKNOWN:
      return std::nullopt;
  }
  return std::nullopt;
}

}  // namespace esphome::fourheat
