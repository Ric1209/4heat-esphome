#include "gsm_modem.h"

#include <string_view>

namespace esphome::fourheat {

static constexpr uint32_t SMS_READ_TIMEOUT_MS = 2500;
static constexpr uint32_t DRAIN_LIMIT_MS = 2500;

void GsmModem::begin(uint32_t now, uint32_t transmit_ms) {
  *this = GsmModem{};
  this->phase_ = Phase::WAIT_READ;
  this->phase_started_ = now;
  this->last_rx_ = now;
  this->sent(now, transmit_ms);
}

void GsmModem::sent(uint32_t now, uint32_t transmit_ms) {
  this->last_tx_ = now;
  this->transmit_ms_ = transmit_ms;
}

void GsmModem::receive(uint8_t byte, uint32_t now) {
  this->last_rx_ = now;
  if (byte == '\r' || byte == '\n') {
    if (!this->invalid_line_) {
      const std::string_view line(this->line_, this->line_size_);
      if (this->phase_ == Phase::WAIT_READ && line == "AT+CMGR=1" &&
          now - this->phase_started_ < this->transmit_ms_ + SMS_READ_TIMEOUT_MS) {
        this->sms_read_ = true;
      }
      if (this->phase_ == Phase::DRAIN && this->sms_sent_ && line == "AT+CMGD=1" &&
          now - this->phase_started_ < DRAIN_LIMIT_MS) {
        this->delete_requested_ = true;
      }
    }
    this->line_size_ = 0;
    this->invalid_line_ = false;
    this->padded_line_ = false;
  } else if (byte == '\0') {
    this->padded_line_ = true;
  } else if (this->padded_line_ || this->line_size_ == sizeof(this->line_)) {
    this->invalid_line_ = true;
  } else {
    this->line_[this->line_size_++] = byte;
  }
}

GsmModem::Event GsmModem::poll(uint32_t now, uint16_t quiet_ms) {
  if (this->phase_ == Phase::WAIT_READ) {
    if (this->sms_read_) {
      this->sms_sent_ = true;
      this->phase_ = Phase::DRAIN;
      this->phase_started_ = now;
      return Event::SEND_SMS;
    }
    if (now - this->phase_started_ < this->transmit_ms_ + SMS_READ_TIMEOUT_MS) {
      return Event::NONE;
    }
    this->phase_ = Phase::DRAIN;
    this->phase_started_ = now;
    this->last_rx_ = now;
  }

  if (this->phase_ == Phase::DRAIN) {
    if (this->delete_requested_) {
      this->delete_requested_ = false;
      return Event::ACK_DELETE;
    }
    const bool quiet = now - this->last_rx_ >= quiet_ms && now - this->last_tx_ >= this->transmit_ms_ + quiet_ms;
    const bool drain_elapsed = now - this->phase_started_ >= DRAIN_LIMIT_MS &&
                               now - this->last_tx_ >= this->transmit_ms_;
    if (quiet || drain_elapsed) {
      this->phase_ = Phase::IDLE;
      return this->sms_sent_ ? Event::DELIVERED : Event::NOT_DELIVERED;
    }
  }
  return Event::NONE;
}

}  // namespace esphome::fourheat
