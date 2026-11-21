#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/alloc_helpers.h"
#include "esphome/core/log.h"
#include "fourheat.h"

#include <algorithm>

namespace esphome {
namespace fourheat {

static const char *const TAG = "fourheat";

static const uint8_t ID_LENGTH = 6;
static const uint8_t DATA_LENGTH = 12;
static const uint8_t MESSAGE_LENGTH = ID_LENGTH + DATA_LENGTH + 2;

static constexpr size_t MAX_RX_BYTES_PER_LOOP = 128;
static constexpr uint32_t POWER_QUERY_INTERVAL_MS = 1000;

void FourHeat::setup() {
  if (this->gsm_enabled_) {
    this->set_module_offline_(true);
  }
}

void FourHeat::loop() {
  for (size_t count = 0; count < MAX_RX_BYTES_PER_LOOP && this->available(); ++count) {
    uint8_t byte;
    if (!this->read_byte(&byte)) {
      break;
    }
    if (this->modem_.active()) {
      this->modem_.receive(byte, millis());
    } else {
      this->handle_char_(byte);
    }
  }

  const auto now = millis();
  if (!this->rx_message_.empty() && now - this->last_rx_char_timestamp_ > this->rx_timeout_) {
    ESP_LOGW(TAG, "Received incomplete data: %s", format_hex_pretty(this->rx_message_).c_str());
    this->rx_message_.clear();
  }

  if (this->power_.expire_verification(now)) {
    ESP_LOGW(TAG, "Power outcome is uncertain after the J30001 verification deadline");
  }

  if (!this->available() && !this->process_modem_(now)) {
    this->process_next_tx_message_();
  }
}

void FourHeat::update() {
  if (this->module_offline_) {
    return;
  }

  ESP_LOGV(TAG, "Updating...");

  if (this->last_sent_query_index_ >= this->queries_.size()) {
    this->last_sent_query_index_ = 0;
  }
}

void FourHeat::dump_config() {
  ESP_LOGCONFIG(TAG, "4Heat:");
}

#ifdef USE_BINARY_SENSOR
void FourHeat::set_module_offline_sensor(binary_sensor::BinarySensor *module_offline_sensor) {
  this->module_offline_sensor_ = module_offline_sensor;
}
#endif

void FourHeat::set_tx_throttle(uint16_t tx_throttle) { this->tx_throttle_ = tx_throttle; }
void FourHeat::set_rx_timeout(uint16_t rx_timeout) { this->rx_timeout_ = rx_timeout; }
void FourHeat::set_response_timeout(uint16_t response_timeout) { this->response_timeout_ = response_timeout; }
void FourHeat::set_retry_throttle(uint16_t retry_throttle) { this->retry_throttle_ = retry_throttle; }
void FourHeat::set_max_retry_count(uint8_t max_retry_count) { this->max_retry_count_ = max_retry_count; }

void FourHeat::register_query(const std::string &id) {
  std::vector<uint8_t> data;

  if (!create_message_(id, {}, data)) {
    return;
  }

  for (const auto &query : this->queries_) {
    if (query == data) {
      return;
    }
  }

  this->queries_.push_back(data);
}

void FourHeat::register_listener(const std::string &id, const std::function<void(const std::vector<uint8_t>)> &func) {
  this->listeners_.push_back(FourHeatDataListener{id, func});
}

void FourHeat::send_raw_value(const std::vector<uint8_t> &value) {
  ESP_LOGV(TAG, "Sending raw data: %s", format_hex_pretty(value).c_str());
  this->tx_queue_.push(value);
}

void FourHeat::send_bytes_value(const std::string &id, const std::vector<uint8_t> &value) {
  std::vector<uint8_t> data;

  if (!create_message_(id, value, data)) {
    return;
  }

  ESP_LOGV(TAG, "Queueing data: %s", format_hex_pretty(data).c_str());
  this->tx_queue_.push(data);
}

void FourHeat::send_string_value(const std::string &id, const std::string &value) {
  std::vector<uint8_t> data(value.begin(), value.end());

  this->send_bytes_value(id, data);
}

void FourHeat::send_integer_value(const std::string &id, uint32_t value) {
  std::string str = std::to_string(value);

  this->send_string_value(id, str);
}

void FourHeat::send_bool_value(const std::string &id, bool value) {
  uint8_t c = value ? '1' : '0';

  this->send_bytes_value(id, {c});
}

// A 4Heat read can answer with the corresponding response prefix. Writes can echo their datapoint.
static bool matches_reply(const std::vector<uint8_t> &request, const std::vector<uint8_t> &reply) {
  if (request.size() < ID_LENGTH + 1 || request[0] != 0x08 ||
      !std::equal(reply.begin() + 1, reply.begin() + ID_LENGTH, request.begin() + 2)) {
    return false;
  }
  const uint8_t prefix = request[1];
  return reply[0] == prefix || (prefix == 'I' && reply[0] == 'J') ||
         (prefix == 'A' && reply[0] == 'B') || (prefix == 'B' && reply[0] == 'A');
}

void FourHeat::handle_char_(uint8_t c) {
  const auto now = millis();

  if (c == 0x08) {
    this->rx_message_.clear();
    return;
  }

  if (c == 0x0D) {
    if (this->gsm_enabled_) {
      // AT traffic and late or unrelated frames must not acknowledge a 4Heat request.
      if (!this->awaiting_response_ || this->tx_ready_(now, this->response_timeout_) ||
          this->rx_message_.size() != ID_LENGTH + DATA_LENGTH ||
          !matches_reply(this->tx_message_, this->rx_message_) ||
          (this->tx_message_ == this->power_status_query_ && this->rx_message_[0] != POWER_STATE_DATAPOINT[0])) {
        this->rx_message_.clear();
        return;
      }
      this->current_retry_count_ = 0;
      if (this->module_offline_) {
        this->set_module_offline_(false);
      }
    }

    this->handle_message_();
    this->rx_message_.clear();
    this->tx_message_.clear();
    this->awaiting_response_ = false;

    if (this->module_offline_) {
      this->set_module_offline_(false);
    }
    return;
  }

  if (this->gsm_enabled_) {
    if (this->rx_message_.empty() && c == '\n') {
      return;
    }
    // Keep one extra byte so an overlong frame cannot pass length validation.
    if (this->rx_message_.size() > ID_LENGTH + DATA_LENGTH) {
      this->last_rx_char_timestamp_ = now;
      return;
    }
  }
  this->rx_message_.push_back(c);
  this->last_rx_char_timestamp_ = now;
}

void FourHeat::handle_message_() {
  auto &data = this->rx_message_;

  if (data.size() != 18) {
    ESP_LOGW(TAG, "Received invalid data: %s", format_hex_pretty(data).c_str());
    return;
  }

  auto it = data.begin() + 6;

  if (*it == 'S' && *(it + 1) == 'Y' && *(it + 2) == 'E' && *(it + 3) == 'V' && *(it + 4) == 'O') {
    it += 5;
  }

  const auto id_end = this->gsm_enabled_ ? data.begin() + 6 : it;
  auto id = std::string(data.begin(), id_end);
  auto value = std::vector<uint8_t>(it, data.end());

  ESP_LOGV(TAG, "Received data for %s: %s", id.c_str(), format_hex_pretty(value).c_str());

  bool handled = false;
  if (this->gsm_enabled_ && id == POWER_STATE_DATAPOINT && this->tx_message_ == this->power_status_query_) {
    this->observe_power_state_(decode_operating_state(value));
    handled = true;
  }

  for (const auto &listener : this->listeners_) {
    if (listener.id == id) {
      handled = true;
      listener.on_data(value);
    }
  }

  if (!handled) {
    ESP_LOGD(TAG, "Listener for %s not found. Data: %s", id.c_str(), format_hex_pretty(value).c_str());
  }
}

bool FourHeat::tx_ready_(uint32_t now, uint32_t delay_ms) const {
  return now - this->last_tx_message_timestamp_ >= this->tx_airtime_ms_ + delay_ms;
}

void FourHeat::transmit_(const uint8_t *data, size_t length) {
  if (this->gsm_enabled_) {
    const auto baud_rate = this->parent_->get_baud_rate();
    const auto bits_per_byte = 1 + this->parent_->get_data_bits() + this->parent_->get_stop_bits() +
                               (this->parent_->get_parity() != uart::UART_CONFIG_PARITY_NONE);
    this->tx_airtime_ms_ = (length * bits_per_byte * 1000 + baud_rate - 1) / baud_rate;
    this->last_tx_message_timestamp_ = millis();
    this->write_array(data, length);
  } else {
    this->write_array(data, length);
    this->flush();
    this->last_tx_message_timestamp_ = millis();
  }
}

void FourHeat::process_next_tx_message_() {
  const auto now = millis();

  if (!this->tx_ready_(now, this->tx_throttle_)) {
    return;
  }

  if (this->awaiting_response_) {
    if (!this->tx_ready_(now, this->response_timeout_)) {
      return;
    }

    this->awaiting_response_ = false;
    if (this->gsm_enabled_) {
      this->rx_message_.clear();
      this->observe_power_state_(OperatingState::UNKNOWN);
    }
    if (this->expedited_power_query_) {
      // A verification read must leave time for normal polling. It has its own cadence.
      this->tx_message_.clear();
    } else if (!this->module_offline_) {
      if (this->current_retry_count_ < this->max_retry_count_) {
        ESP_LOGW(TAG, "Response timeout, retrying (%d/%d)", this->current_retry_count_, this->max_retry_count_);
        this->current_retry_count_++;
      } else {
        ESP_LOGW(TAG, "Response timeout, giving up");
        this->current_retry_count_ = 0;
        this->tx_message_.clear();
        ESP_LOGW(TAG, "Clearing command TX queue, because the module can reappear in an unknown state");
        this->tx_queue_ = {};
        this->set_module_offline_(true);
      }
    }
  }

  if (this->module_offline_ && !this->tx_ready_(now, this->retry_throttle_)) {
    return;
  }

  if (this->gsm_enabled_ && !this->rx_message_.empty()) {
    return;
  }

  if (this->tx_message_.empty() && this->start_gsm_exchange_()) {
    return;
  }

  auto &data = this->tx_message_;
  this->expedited_power_query_ = false;
  if (!data.empty()) {
    ESP_LOGV(TAG, "Resending data: %s", format_hex_pretty(data).c_str());
  } else if (this->gsm_enabled_ && !this->module_offline_ &&
             (this->power_.queued() || this->refresh_power_state_ ||
              (this->power_.verifying() && now - this->last_power_query_ >= POWER_QUERY_INTERVAL_MS))) {
    data = this->power_status_query_;
    this->refresh_power_state_ = false;
    this->expedited_power_query_ = true;
  } else if (!this->tx_queue_.empty()) {
    data = this->tx_queue_.front();
    this->tx_queue_.pop();

    ESP_LOGV(TAG, "Sending data: %s", format_hex_pretty(data).c_str());
  } else if (this->last_sent_query_index_ < this->queries_.size()) {
    data = this->queries_[this->last_sent_query_index_];
    ESP_LOGV(TAG, "Sending query (index %d): %s", this->last_sent_query_index_, format_hex_pretty(data).c_str());

    this->last_sent_query_index_++;
  } else {
    return;
  }

  this->transmit_(data.data(), data.size());
  if (this->gsm_enabled_ && data == this->power_status_query_) {
    this->last_power_query_ = this->last_tx_message_timestamp_;
  }
  this->awaiting_response_ = true;
}

bool FourHeat::create_message_(const std::string &id, const std::vector<uint8_t> &value, std::vector<uint8_t> &message) {
  if (value.size() > DATA_LENGTH) {
    ESP_LOGW(TAG, "Value too long: %s", format_hex_pretty(value).c_str());
    return false;
  }

  std::vector<uint8_t> data{0x08};

  data.insert(data.end(), id.begin(), id.end());

  if (value.size() < DATA_LENGTH) {
    data.insert(data.end(), DATA_LENGTH - value.size(), '0');
  }

  data.insert(data.end(), value.begin(), value.end());

  data.push_back(0x0D);

  message = data;
  return true;
}

void FourHeat::set_module_offline_(bool offline) {
  this->module_offline_ = offline;
  if (offline && this->gsm_enabled_) {
    // Restart the query sequence after an outage, even if the last query had failed.
    this->last_sent_query_index_ = 0;
    this->observe_power_state_(OperatingState::UNKNOWN);
  }

#ifdef USE_BINARY_SENSOR
  if (this->module_offline_sensor_ != nullptr) {
    this->module_offline_sensor_->publish_state(offline);
  }
#endif
}

}  // namespace fourheat
}  // namespace esphome
