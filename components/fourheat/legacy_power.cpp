#include "fourheat.h"

#include <algorithm>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace fourheat {

static const char *const TAG = "fourheat.legacy_power";
static constexpr uint32_t SMS_READ_TIMEOUT_MS = 2500;
static constexpr uint32_t MODEM_DRAIN_LIMIT_MS = 2500;
static constexpr uint32_t VERIFICATION_INTERVAL_MS = 1000;
static constexpr uint32_t VERIFICATION_LIMIT_MS = 300000;

LegacyPowerStatus classify_legacy_power_state(std::vector<uint8_t>::const_iterator begin,
                                              std::vector<uint8_t>::const_iterator end) {
  if (end - begin >= 5 && std::equal(begin, begin + 5, "SYEVO")) {
    begin += 5;
  }
  if (begin == end) {
    return LegacyPowerStatus::UNKNOWN;
  }
  unsigned state = 0;
  for (; begin != end; ++begin) {
    if (*begin < '0' || *begin > '9' || state > 34) {
      return LegacyPowerStatus::UNKNOWN;
    }
    state = state * 10 + (*begin - '0');
  }
  switch (state) {
    case 0:
      return LegacyPowerStatus::OFF;
    case 1:
    case 2:
    case 3:
    case 4:
    case 10:
    case 30:
    case 31:
    case 32:
    case 33:
    case 34:
      return LegacyPowerStatus::STARTING;
    case 5:
    case 6:
      return LegacyPowerStatus::RUNNING;
    case 7:
      return LegacyPowerStatus::STOPPING;
    case 8:
    case 9:
      return LegacyPowerStatus::ALARM;
    case 11:
      return LegacyPowerStatus::STANDBY;
    default:
      return LegacyPowerStatus::UNKNOWN;
  }
}

void FourHeat::request_legacy_power(bool on) {
  if (this->module_offline_) {
    ESP_LOGW(TAG, "Legacy power request rejected while the controller is offline");
    return;
  }
  if (!on && this->legacy_target_on_ &&
      (this->legacy_phase_ == LegacyPhase::VERIFY_PENDING || this->legacy_phase_ == LegacyPhase::VERIFY_REPLY)) {
    this->legacy_stop_pending_ = true;
    return;
  }
  if (this->legacy_phase_ != LegacyPhase::IDLE) {
    ESP_LOGW(TAG, "Legacy power request rejected while another request is in progress");
    return;
  }
  this->legacy_target_on_ = on;
  this->legacy_phase_ = LegacyPhase::QUEUED;
}

void FourHeat::write_legacy_bytes_(const std::string &data) {
  this->write_array(reinterpret_cast<const uint8_t *>(data.data()), data.size());
  this->flush();
  this->last_tx_message_timestamp_ = millis();
}

bool FourHeat::start_legacy_exchange_if_due_(uint32_t now) {
  if (this->module_offline_ || !this->rx_message_.empty()) {
    return false;
  }
  if (this->legacy_phase_ == LegacyPhase::QUEUED) {
    this->legacy_line_.clear();
    this->legacy_line_invalid_ = false;
    this->legacy_line_padded_ = false;
    this->legacy_sms_delivered_ = false;
    this->legacy_phase_ = LegacyPhase::WAIT_READ_SMS;
    this->write_legacy_bytes_("\r\n+CMTI: \"SM\",1\r\n");
    this->legacy_phase_started_ = this->last_tx_message_timestamp_;
    return true;
  }
  if (this->legacy_phase_ == LegacyPhase::VERIFY_PENDING &&
      now - this->legacy_last_read_ >= VERIFICATION_INTERVAL_MS) {
    std::vector<uint8_t> frame;
    this->create_message_("I30001", {}, frame);
    this->legacy_phase_ = LegacyPhase::VERIFY_REPLY;
    this->write_array(frame);
    this->flush();
    this->last_tx_message_timestamp_ = millis();
    this->legacy_last_read_ = this->last_tx_message_timestamp_;
    return true;
  }
  return false;
}

void FourHeat::handle_legacy_modem_byte_(uint8_t c) {
  this->last_rx_char_timestamp_ = millis();
  if (c == '\r' || c == '\n') {
    if (!this->legacy_line_invalid_ && this->legacy_phase_ == LegacyPhase::WAIT_READ_SMS &&
        this->legacy_line_ == "AT+CMGR=1" &&
        this->last_rx_char_timestamp_ - this->legacy_phase_started_ < SMS_READ_TIMEOUT_MS) {
      this->legacy_phase_ = LegacyPhase::SEND_SMS;
    } else if (!this->legacy_line_.empty()) {
      ESP_LOGD(TAG, "Ignoring unsupported modem request during legacy exchange");
    }
    this->legacy_line_.clear();
    this->legacy_line_invalid_ = false;
    this->legacy_line_padded_ = false;
    return;
  }
  if (c == '\0') {
    // The capture has trailing NUL bytes before CR.
    this->legacy_line_padded_ = true;
    return;
  }
  if (this->legacy_line_padded_ || this->legacy_line_.size() == 64) {
    this->legacy_line_invalid_ = true;
    return;
  }
  this->legacy_line_.push_back(c);
}

bool FourHeat::process_legacy_power_(uint32_t now) {
  if (this->legacy_phase_ == LegacyPhase::SEND_SMS) {
    const char *command = this->legacy_target_on_ ? "Start" : "Stop";
    this->write_legacy_bytes_(std::string("\r\n+CMGR: \"REC UNREAD\",\"+3911111111\",,\r\n") +
                              command + "\r\n\r\nOK\r\n");
    this->legacy_sms_delivered_ = true;
    this->legacy_phase_ = LegacyPhase::DRAIN;
    this->legacy_phase_started_ = this->last_tx_message_timestamp_;
    this->last_rx_char_timestamp_ = this->last_tx_message_timestamp_;
    return true;
  }

  if (this->legacy_phase_ == LegacyPhase::WAIT_READ_SMS) {
    if (now - this->legacy_phase_started_ < SMS_READ_TIMEOUT_MS) {
      return true;
    }
    ESP_LOGW(TAG, "Legacy power request timed out waiting for AT+CMGR");
    this->legacy_phase_ = LegacyPhase::DRAIN;
    this->legacy_phase_started_ = now;
  }

  if (this->legacy_phase_ == LegacyPhase::DRAIN) {
    if (now - this->last_rx_char_timestamp_ < this->rx_timeout_ &&
        now - this->legacy_phase_started_ < MODEM_DRAIN_LIMIT_MS) {
      return true;
    }
    this->legacy_line_.clear();
    this->legacy_phase_ = this->legacy_sms_delivered_ ? LegacyPhase::VERIFY_PENDING : LegacyPhase::IDLE;
    this->legacy_verification_started_ = now;
    this->legacy_last_read_ = now - VERIFICATION_INTERVAL_MS;
    this->legacy_last_status_ = LegacyPowerStatus::UNKNOWN;
  }

  if (this->legacy_phase_ == LegacyPhase::VERIFY_REPLY) {
    if (now - this->legacy_last_read_ < this->response_timeout_) {
      return true;
    }
    this->rx_message_.clear();
    this->legacy_phase_ = LegacyPhase::VERIFY_PENDING;
  }

  if (this->legacy_stop_pending_) {
    this->legacy_stop_pending_ = false;
    this->legacy_target_on_ = false;
    this->legacy_phase_ = LegacyPhase::QUEUED;
  }

  if (this->legacy_phase_ == LegacyPhase::VERIFY_PENDING &&
      now - this->legacy_verification_started_ >= VERIFICATION_LIMIT_MS) {
    ESP_LOGW(TAG, "Legacy power outcome unconfirmed by J30001");
    this->legacy_phase_ = LegacyPhase::IDLE;
  }
  return false;
}

void FourHeat::handle_legacy_state_(uint32_t now) {
  const auto &frame = this->rx_message_;
  if (now - this->legacy_last_read_ >= this->response_timeout_ ||
      frame.size() != 18 || !std::equal(frame.begin(), frame.begin() + 6, "J30001")) {
    return;
  }
  const auto status = classify_legacy_power_state(frame.begin() + 6, frame.end());
  this->handle_message_();
  const auto target = this->legacy_target_on_ ? LegacyPowerStatus::RUNNING : LegacyPowerStatus::OFF;
  if (status == target) {
    ESP_LOGI(TAG, "Legacy power target observed in J30001: %s", this->legacy_target_on_ ? "running" : "off");
    this->legacy_phase_ = LegacyPhase::IDLE;
  } else if (status == LegacyPowerStatus::ALARM) {
    ESP_LOGW(TAG, "Legacy power verification ended in Safety or Block");
    this->legacy_phase_ = LegacyPhase::IDLE;
  } else {
    if (status != this->legacy_last_status_) {
      if (status == LegacyPowerStatus::STARTING) {
        ESP_LOGI(TAG, "J30001 reports startup in progress");
      } else if (status == LegacyPowerStatus::STOPPING) {
        ESP_LOGI(TAG, "J30001 reports shutdown in progress");
      } else if (status == LegacyPowerStatus::STANDBY) {
        ESP_LOGI(TAG, "J30001 reports standby");
      }
    }
    this->legacy_last_status_ = status;
    this->legacy_phase_ = LegacyPhase::VERIFY_PENDING;
  }
}

}  // namespace fourheat
}  // namespace esphome
