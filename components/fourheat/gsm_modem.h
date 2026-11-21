#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome::fourheat {

class GsmModem {
 public:
  enum class Event { NONE, SEND_SMS, ACK_DELETE, DELIVERED, NOT_DELIVERED };

  static constexpr char NOTIFICATION[] = "\r\n+CMTI: \"SM\",1\r\n";
  static constexpr char START_SMS[] = "\r\n+CMGR: \"REC UNREAD\",\"+3911111111\",,\r\nStart\r\n\r\nOK\r\n";
  static constexpr char STOP_SMS[] = "\r\n+CMGR: \"REC UNREAD\",\"+3911111111\",,\r\nStop\r\n\r\nOK\r\n";
  static constexpr char OK[] = "\r\nOK\r\n";

  bool active() const { return this->phase_ != Phase::IDLE; }
  void begin(uint32_t now, uint32_t transmit_ms);
  void receive(uint8_t byte, uint32_t now);
  Event poll(uint32_t now, uint16_t quiet_ms);
  void sent(uint32_t now, uint32_t transmit_ms);

 private:
  enum class Phase { IDLE, WAIT_READ, DRAIN };
  Phase phase_{Phase::IDLE};
  bool sms_read_{false};
  bool sms_sent_{false};
  bool delete_requested_{false};
  uint32_t phase_started_{0};
  uint32_t last_rx_{0};
  uint32_t last_tx_{0};
  uint32_t transmit_ms_{0};

  // CMGR and CMGD have the same length. Allow trailing NUL padding before CR.
  char line_[sizeof("AT+CMGR=1") - 1];
  size_t line_size_{0};
  bool invalid_line_{false};
  bool padded_line_{false};
};

}  // namespace esphome::fourheat
