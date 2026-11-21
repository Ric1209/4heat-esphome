#pragma once

#include "operating_state.h"

namespace esphome::fourheat {

class GsmPower {
 public:
  bool request(bool on);
  bool queued() const { return this->phase_ == Phase::QUEUED || this->ready(); }
  bool ready() const { return this->phase_ == Phase::READY; }
  bool verifying() const { return this->phase_ == Phase::VERIFYING; }
  bool target_on() const { return this->target_on_; }
  void begin_exchange() { this->phase_ = Phase::EXCHANGING; }
  void finish_exchange(bool delivered, uint32_t now);
  void observe(OperatingState state);
  bool expire_verification(uint32_t now);
  bool uncertain() const;
  std::optional<bool> switch_state() const;

 private:
  enum class Phase { IDLE, QUEUED, READY, EXCHANGING, VERIFYING, UNCERTAIN };
  Phase phase_{Phase::IDLE};
  OperatingState state_{OperatingState::UNKNOWN};
  bool target_on_{false};
  uint32_t verification_started_{0};

  bool can_request_(bool on) const;
};

}  // namespace esphome::fourheat
