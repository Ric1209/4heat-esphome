#include "gsm_power.h"

namespace esphome::fourheat {

static constexpr uint32_t VERIFICATION_LIMIT_MS = 300000;

bool GsmPower::can_request_(bool on) const { return on ? can_start(this->state_) : can_stop(this->state_); }

bool GsmPower::request(bool on) {
  if (this->phase_ != Phase::IDLE || !this->can_request_(on)) {
    return false;
  }
  this->target_on_ = on;
  this->phase_ = Phase::QUEUED;
  return true;
}

void GsmPower::finish_exchange(bool delivered, uint32_t now) {
  this->phase_ = delivered ? Phase::VERIFYING : Phase::IDLE;
  this->verification_started_ = now;
}

void GsmPower::observe(OperatingState state) {
  this->state_ = state;
  if (this->queued()) {
    this->phase_ = this->can_request_(this->target_on_) ? Phase::READY : Phase::IDLE;
  }
  if (this->phase_ == Phase::VERIFYING || this->phase_ == Phase::UNCERTAIN) {
    if (is_alarm(state) || power_direction(state) == this->target_on_) {
      this->phase_ = Phase::IDLE;
    }
  }
}

bool GsmPower::expire_verification(uint32_t now) {
  if (this->verifying() && now - this->verification_started_ >= VERIFICATION_LIMIT_MS) {
    this->phase_ = Phase::UNCERTAIN;
    return true;
  }
  return false;
}

bool GsmPower::uncertain() const {
  return this->phase_ != Phase::IDLE || !power_direction(this->state_).has_value();
}

std::optional<bool> GsmPower::switch_state() const {
  if (this->phase_ != Phase::IDLE) {
    return this->target_on_;
  }
  return power_direction(this->state_);
}

}  // namespace esphome::fourheat
