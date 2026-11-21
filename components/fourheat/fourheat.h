#pragma once

#include <functional>
#include <queue>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/defines.h"

#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif

#include "esphome/components/uart/uart.h"
#include "gsm_modem.h"
#include "gsm_power.h"

namespace esphome {
namespace fourheat {

struct FourHeatDataListener {
  std::string id;
  std::function<void(const std::vector<uint8_t> &)> on_data;
};

class FourHeat : public PollingComponent, public uart::UARTDevice {
 public:
  float get_setup_priority() const override { return setup_priority::LATE; }

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;

#ifdef USE_BINARY_SENSOR
  void set_module_offline_sensor(binary_sensor::BinarySensor *module_offline_sensor);
  void set_power_state_uncertain_sensor(binary_sensor::BinarySensor *sensor) { this->power_state_uncertain_sensor_ = sensor; }
#endif

  void set_tx_throttle(uint16_t tx_throttle);
  void set_rx_timeout(uint16_t rx_timeout);
  void set_response_timeout(uint16_t response_timeout);
  void set_retry_throttle(uint16_t retry_throttle);
  void set_max_retry_count(uint8_t max_retry_count);

  void register_query(const std::string &id);
  void register_listener(const std::string &id, const std::function<void(const std::vector<uint8_t>)> &func);

  void register_gsm_power_switch(std::function<void(bool)> publish_state);
  void request_gsm_power(bool on);

  void send_raw_value(const std::vector<uint8_t> &value);
  void send_bytes_value(const std::string &id, const std::vector<uint8_t> &value);
  void send_string_value(const std::string &id, const std::string &value);
  void send_integer_value(const std::string &id, uint32_t value);
  void send_bool_value(const std::string &id, bool value);

 protected:
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *module_offline_sensor_{nullptr};
  binary_sensor::BinarySensor *power_state_uncertain_sensor_{nullptr};
#endif

  uint16_t tx_throttle_{10};
  uint16_t rx_timeout_{100};
  uint16_t response_timeout_{300};
  uint16_t retry_throttle_{1000};
  uint8_t max_retry_count_{3};

  uint32_t last_rx_char_timestamp_{0};
  uint32_t last_tx_message_timestamp_{0};
  uint32_t tx_airtime_ms_{0};
  std::vector<uint8_t> rx_message_;

  bool awaiting_response_{false};
  bool module_offline_{true};
  uint8_t current_retry_count_{0};
  uint8_t last_sent_query_index_{0};
  std::vector<uint8_t> tx_message_;

  std::vector<std::vector<uint8_t>> queries_;
  std::queue<std::vector<uint8_t>> tx_queue_;
  std::vector<FourHeatDataListener> listeners_;

  bool gsm_enabled_{false};
  GsmModem modem_;
  GsmPower power_;
  std::vector<std::function<void(bool)>> power_switches_;
  std::vector<uint8_t> power_status_query_;
  uint32_t last_power_query_{0};
  bool refresh_power_state_{false};
  bool expedited_power_query_{false};

  bool process_modem_(uint32_t now);
  bool start_gsm_exchange_();
  void observe_power_state_(OperatingState state);
  void publish_power_state_();

  void transmit_(const uint8_t *data, size_t length);
  bool tx_ready_(uint32_t now, uint32_t delay_ms) const;

  void handle_char_(uint8_t c);
  void handle_message_();
  void process_next_tx_message_();

  bool create_message_(const std::string &id, const std::vector<uint8_t> &value, std::vector<uint8_t> &message);
  void set_module_offline_(bool offline);
};

}  // namespace fourheat
}  // namespace esphome
