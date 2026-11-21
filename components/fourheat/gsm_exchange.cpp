#include "fourheat.h"

#include <cstring>
#include <utility>

#include "esphome/core/log.h"

namespace esphome::fourheat {

static const char *const TAG = "fourheat.gsm";

void FourHeat::register_gsm_power_switch(std::function<void(bool)> publish_state) {
  this->gsm_enabled_ = true;
  this->power_switches_.push_back(std::move(publish_state));
  this->register_query(POWER_STATE_QUERY);
  this->create_message_(POWER_STATE_QUERY, {}, this->power_status_query_);
}

void FourHeat::request_gsm_power(bool on) {
  if (!this->module_offline_ && this->power_.request(on)) {
    this->publish_power_state_();
  } else {
    ESP_LOGD(TAG, "Power request ignored for the current controller state");
  }
}

void FourHeat::publish_power_state_() {
#ifdef USE_BINARY_SENSOR
  if (this->power_state_uncertain_sensor_ != nullptr) {
    this->power_state_uncertain_sensor_->publish_state(this->power_.uncertain());
  }
#endif
  if (const auto state = this->power_.switch_state(); state.has_value()) {
    for (const auto &publish_state : this->power_switches_) {
      publish_state(*state);
    }
  }
}

void FourHeat::observe_power_state_(OperatingState state) {
  const bool queued = this->power_.queued();
  const bool verifying = this->power_.verifying();
  this->power_.observe(state);
  if (queued && !this->power_.queued()) {
    ESP_LOGW(TAG, "Power request cancelled before sending the SMS notification");
  }
  if (verifying && !this->power_.verifying()) {
    if (is_alarm(state)) {
      ESP_LOGW(TAG, "Power verification ended in Safety or Block");
    } else {
      ESP_LOGI(TAG, "Power direction confirmed by J30001");
    }
  }
  this->publish_power_state_();
}

bool FourHeat::start_gsm_exchange_() {
  if (!this->power_.ready() || this->module_offline_) {
    return false;
  }
  this->power_.begin_exchange();
  this->transmit_(reinterpret_cast<const uint8_t *>(GsmModem::NOTIFICATION), sizeof(GsmModem::NOTIFICATION) - 1);
  this->modem_.begin(this->last_tx_message_timestamp_, this->tx_airtime_ms_);
  return true;
}

bool FourHeat::process_modem_(uint32_t now) {
  if (!this->modem_.active()) {
    return false;
  }

  if (!this->tx_ready_(now, 0)) {
    return true;
  }

  const char *reply = nullptr;
  switch (this->modem_.poll(now, this->rx_timeout_)) {
    case GsmModem::Event::NONE:
      return true;
    case GsmModem::Event::SEND_SMS:
      reply = this->power_.target_on() ? GsmModem::START_SMS : GsmModem::STOP_SMS;
      break;
    case GsmModem::Event::ACK_DELETE:
      reply = GsmModem::OK;
      break;
    case GsmModem::Event::DELIVERED:
      this->power_.finish_exchange(true, now);
      this->refresh_power_state_ = true;
      return false;
    case GsmModem::Event::NOT_DELIVERED:
      ESP_LOGW(TAG, "Power request timed out waiting for AT+CMGR");
      this->power_.finish_exchange(false, now);
      this->refresh_power_state_ = true;
      this->publish_power_state_();
      return false;
  }

  this->transmit_(reinterpret_cast<const uint8_t *>(reply), std::strlen(reply));
  this->modem_.sent(this->last_tx_message_timestamp_, this->tx_airtime_ms_);
  return true;
}

}  // namespace esphome::fourheat
