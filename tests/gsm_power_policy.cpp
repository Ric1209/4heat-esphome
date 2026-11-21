#include "gsm_modem.h"
#include "gsm_power.h"

#include <cassert>
#include <string_view>

using namespace esphome::fourheat;

static void receive(GsmModem &modem, std::string_view command, uint32_t now) {
  for (const auto byte : command) {
    modem.receive(byte, now);
  }
}

static void test_modem_drain_and_timeout() {
  using Event = GsmModem::Event;
  GsmModem modem;
  modem.begin(0, 17);
  receive(modem, "AT+CMGR=2\r", 20);
  assert(modem.poll(20, 100) == Event::NONE);
  receive(modem, std::string_view("AT+CMGR=1\0\0\r", 12), 30);
  assert(modem.poll(30, 100) == Event::SEND_SMS);
  modem.sent(30, 50);
  assert(modem.poll(179, 100) == Event::NONE);
  receive(modem, "AT+CMGD=2\rAT+CMGR=1\r", 179);
  assert(modem.poll(180, 100) == Event::NONE);
  receive(modem, std::string_view("AT+CMGD=1\0\0\r", 12), 180);
  assert(modem.poll(180, 100) == Event::ACK_DELETE);
  modem.sent(180, 7);
  assert(modem.poll(286, 100) == Event::NONE);
  assert(modem.poll(287, 100) == Event::DELIVERED);
  assert(!modem.active());

  modem.begin(1000, 17);
  receive(modem, "AT+CMGR=1\r", 3517);
  assert(modem.poll(3517, 100) == Event::NONE);
  receive(modem, "AT+CMGR=1\rAT+CMGD=1\r", 3600);
  assert(modem.poll(3700, 100) == Event::NOT_DELIVERED);
}

static void test_uncertain_power_outcome() {
  GsmPower power;
  assert(power.uncertain());
  assert(!power.request(true));
  power.observe(OperatingState::OFF);
  assert(power.switch_state() == false);
  assert(power.request(true));
  assert(power.switch_state() == true);
  assert(!power.request(true));
  assert(!power.request(false));
  assert(!power.ready());
  power.observe(OperatingState::OFF);
  assert(power.ready());
  power.begin_exchange();
  power.finish_exchange(true, 1000);
  power.observe(OperatingState::OFF);
  assert(!power.expire_verification(300999));
  assert(power.expire_verification(301000));
  assert(!power.verifying() && power.uncertain());
  assert(power.switch_state() == true);
  power.observe(OperatingState::OFF);
  assert(!power.request(true));

  power.observe(OperatingState::CHECK_UP);
  assert(!power.uncertain());
  assert(power.switch_state() == true);
  assert(!power.request(true) && !power.request(false));
  power.observe(OperatingState::MODULATION);
  assert(power.request(false));
  power.observe(OperatingState::MODULATION);
  assert(power.ready());
  power.begin_exchange();
  power.finish_exchange(true, 301100);
  power.observe(OperatingState::UNKNOWN);
  assert(power.uncertain() && power.switch_state() == false);
  power.observe(OperatingState::SHUTDOWN);
  assert(!power.uncertain() && power.switch_state() == false);
  assert(!power.request(true) && !power.request(false));
}

static void test_numeric_states() {
  for (const auto *code : {"000", "0005", "00011", "0030", "0034", "0000010"}) {
    const std::string_view value(code);
    assert(decode_operating_state({value.begin(), value.end()}) != OperatingState::UNKNOWN);
  }
  for (const auto *code : {"", "999999999999", "00035", "00012", " 1", "00-1"}) {
    const std::string_view value(code);
    assert(decode_operating_state({value.begin(), value.end()}) == OperatingState::UNKNOWN);
  }
  assert(power_direction(OperatingState::STANDBY) == true);
  assert(!can_start(OperatingState::STANDBY) && !can_stop(OperatingState::STANDBY));
  assert(power_direction(OperatingState::IGNITION_RECOVERY) == true);
  assert(!can_start(OperatingState::IGNITION_RECOVERY) && !can_stop(OperatingState::IGNITION_RECOVERY));
}

int main() {
  test_modem_drain_and_timeout();
  test_uncertain_power_outcome();
  test_numeric_states();
}
