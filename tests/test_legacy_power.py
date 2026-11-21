"""Exercise the real ESPHome host component against a simulated RS232 peer."""

import os
from pathlib import Path
import pty
import re
import select
import subprocess
import tempfile
import time
import tty
import unittest

ROOT = Path(__file__).resolve().parents[1]
CMTI = b'+CMTI: "SM",1'
SMS = b'+CMGR: "REC UNREAD","+3911111111",,'


def frame(datapoint, value=0):
    return b"\x08" + datapoint + f"{value:012d}".encode() + b"\r"


class SimulatedController:
    def __init__(self, program, port, commands):
        self.pty, self.slave = self.open_port(port)
        self.command_pty, self.command_slave = self.open_port(commands)
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen(
            [program], stdout=self.log, stderr=subprocess.STDOUT
        )
        self.buffer = b""

    @staticmethod
    def open_port(path):
        master, slave = pty.openpty()
        tty.setraw(slave)
        path.unlink(missing_ok=True)
        path.symlink_to(os.ttyname(slave))
        return master, slave

    def close(self):
        if self.log.closed:
            return
        self.process.terminate()
        self.process.wait(timeout=5)
        for fd in (self.pty, self.slave, self.command_pty, self.command_slave):
            os.close(fd)
        self.log.close()

    def request(self, commands):
        os.write(self.command_pty, commands)

    def write(self, data):
        os.write(self.pty, data)

    def read(self, timeout=3):
        deadline = time.monotonic() + timeout
        while b"\r" not in self.buffer:
            remaining = max(0, deadline - time.monotonic())
            if not select.select([self.pty], [], [], remaining)[0]:
                return None
            self.buffer += os.read(self.pty, 1024)
        line, self.buffer = self.buffer.split(b"\r", 1)
        return line.strip(b"\n")

    def expect(self, test, expected, timeout=4, auto_reply=None):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.read(max(0, deadline - time.monotonic()))
            if line == expected:
                return
            if line and auto_reply is not None and line.startswith(b"\x08I"):
                self.reply_to_query(line, auto_reply)
            elif line:
                test.fail(f"Unexpected UART line {line!r}, wanted {expected!r}")
        test.fail(f"Timed out waiting for {expected!r}. Host logs:\n{self.logs()}")

    def reply_to_query(self, line, state):
        datapoint = line[2:7]
        self.write(frame(b"J" + datapoint, state if datapoint == b"30001" else 23))

    def logs(self):
        return os.pread(self.log.fileno(), 100000, 0).decode(errors="replace")


class PowerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(temp.cleanup)
        cls.directory = Path(temp.name)
        cls.port = Path("/tmp") / f"{cls.directory.name}-fourheat-controller"
        cls.commands = Path("/tmp") / f"{cls.directory.name}-fourheat-commands"
        cls.addClassCleanup(cls.port.unlink, missing_ok=True)
        cls.addClassCleanup(cls.commands.unlink, missing_ok=True)
        cls.programs = {}
        for mode in ("gsm", "standard"):
            config = cls.write_config(mode)
            subprocess.run(["esphome", "compile", config], check=True)
            name = config.stem
            cls.programs[mode] = cls.directory / name / ".pioenvs" / name / "program"

    @classmethod
    def write_config(cls, mode):
        name = f"fourheat-{mode}"
        config = cls.directory / f"{name}.yaml"
        if mode == "gsm":
            power_options = "    power_control: gsm"
        else:
            power_options = """    datapoint: J30001
    on_datapoint: J30253
    off_datapoint: J30254
    on_data: '1'
    off_data: '1'
    parser: return data.back() == '5';"""
        config.write_text(f"""
esphome:
  name: {name}
  build_path: {cls.directory / name}
host:
logger:
  level: DEBUG
external_components:
  - source:
      type: local
      path: {ROOT / 'components'}
uart:
  - id: controller_bus
    port: {cls.port}
    baud_rate: 9600
  - id: commands
    port: {cls.commands}
    baud_rate: 9600
fourheat:
  id: bus
  uart_id: controller_bus
  update_interval: 220ms
  response_timeout: 200
  retry_throttle: 500
  max_retry_count: 1
interval:
  - interval: 10ms
    then:
      - lambda: |-
          uint8_t byte;
          while (id(commands).available() && id(commands).read_byte(&byte)) {{
            if (byte == '+') id(heater).turn_on();
            if (byte == '-') id(heater).turn_off();
            if (byte == 'w') id(bus).send_string_value("B20364", "1");
          }}
switch:
  - platform: fourheat
    id: heater
    name: Heater
    restore_mode: DISABLED
{power_options}
    on_turn_on:
      - logger.log: "POWER ON"
    on_turn_off:
      - logger.log: "POWER OFF"
binary_sensor:
  - platform: fourheat
    type: power_state_uncertain
    name: Power uncertain
    trigger_on_initial_state: true
    on_press:
      - logger.log: "UNCERTAIN YES"
    on_release:
      - logger.log: "UNCERTAIN NO"
  - platform: fourheat
    type: module_offline
    name: Offline
    trigger_on_initial_state: true
    on_press:
      - logger.log: "OFFLINE YES"
text_sensor:
  - platform: fourheat
    name: English
    on_value:
      - logger.log:
          format: "English %s"
          args: [x.c_str()]
    datapoint: J30001
    options:
      0: 'OFF'
      1: 'Check Up'
      5: 'Run'
      7: 'Extinguishing'
      99: 'ON'
  - platform: fourheat
    name: Italian
    on_value:
      - logger.log:
          format: "Italian %s"
          args: [x.c_str()]
    datapoint: J30001
    options:
      0: 'Spento'
      1: 'Check Up'
      5: 'Normale'
      7: 'Spegnimento'
      99: 'Normale'
sensor:
  - platform: fourheat
    name: Exhaust
    datapoint: J30005
""")
        return config

    def controller(self, mode="gsm"):
        controller = SimulatedController(self.programs[mode], self.port, self.commands)
        self.addCleanup(controller.close)
        return controller

    def observe(self, controller, state, previous=None, syevo=False):
        controller.expect(self, frame(b"I30001")[:-1], auto_reply=state if previous is None else previous)
        if syevo:
            controller.write(b"\x08J30001SYEVO" + f"{state:07d}".encode() + b"\r")
        else:
            controller.write(frame(b"J30001", state))

    def exchange(self, controller, command, state, delete=True):
        controller.expect(self, CMTI, auto_reply=state)
        controller.write(b"AT+CMGR=2\rAT+CMGR=\x001\rAT+CM")
        self.assertIsNone(controller.read(0.07))
        controller.write(b"GR=1\x00\x00\r\n")
        for line in (SMS, command, b"OK"):
            controller.expect(self, line)
        if delete:
            controller.write(b"AT+CMGD=2\rAT+CMGR=1\rAT+CMGD=1\x00\x00\r")
            controller.expect(self, b"OK")

    def assert_only_polls(self, controller, state, duration=0.45):
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            line = controller.read(max(0, deadline - time.monotonic()))
            if line:
                self.assertIn(line, (frame(b"I30001")[:-1], frame(b"I30005")[:-1]))
                controller.reply_to_query(line, state)

    def test_start_and_stop_follow_direction_and_never_repeat(self):
        controller = self.controller()
        self.observe(controller, 0)
        self.wait_for_log(controller, "POWER OFF")
        controller.request(b"++-")
        controller.expect(self, CMTI, auto_reply=0)
        self.wait_for_log(controller, "POWER ON")
        controller.request(b"+w")
        self.assertIsNone(controller.read(0.07))
        controller.write(b"AT+CMGR=1\x00\x00\r")
        for line in (SMS, b"Start", b"OK"):
            controller.expect(self, line)
        controller.write(b"AT+CMGD=1\x00\x00\r")
        controller.expect(self, b"OK")
        self.observe(controller, 0)  # The old state can still arrive after the SMS.
        controller.expect(self, frame(b"B20364", 1)[:-1], auto_reply=0)
        controller.write(frame(b"B20364", 1))
        self.assertEqual(self.switch_states(controller), ["OFF", "ON"])
        self.observe(controller, 1)
        self.wait_for_log(controller, "Power direction confirmed by J30001")
        for state in (2, 3, 4, 10, 30, 31, 32, 33, 34):
            controller.request(b"+-")
            self.assert_only_polls(controller, state, duration=0.25)
        self.observe(controller, 5, syevo=True)
        self.wait_for_log(controller, "Italian Normale")
        self.wait_for_log(controller, "English Run")
        self.assertEqual(self.switch_states(controller), ["OFF", "ON"])
        controller.request(b"--+")
        self.exchange(controller, b"Stop", 5)
        self.wait_for_log(controller, "POWER OFF", count=2)
        self.observe(controller, 5)
        self.assertEqual(self.switch_states(controller), ["OFF", "ON", "OFF"])
        self.observe(controller, 7)
        self.wait_for_log(controller, "Power direction confirmed by J30001", count=2)
        controller.request(b"+-")
        self.assert_only_polls(controller, 7)
        self.observe(controller, 0)
        self.assert_only_polls(controller, 0)
        self.assertEqual(self.switch_states(controller), ["OFF", "ON", "OFF"])

    def test_panel_changes_and_restart_during_transitions(self):
        controller = self.controller()
        self.observe(controller, 1)
        self.wait_for_log(controller, "POWER ON")
        self.observe(controller, 11)
        controller.request(b"+-")
        self.assert_only_polls(controller, 11)
        self.observe(controller, 6)
        self.observe(controller, 7)
        self.wait_for_log(controller, "POWER OFF")
        self.observe(controller, 0)
        self.assertEqual(self.switch_states(controller), ["ON", "OFF"])

        controller.close()
        controller = self.controller()
        self.observe(controller, 7)
        self.wait_for_log(controller, "POWER OFF")
        controller.request(b"+-")
        self.assert_only_polls(controller, 7)
        self.assertEqual(self.switch_states(controller), ["OFF"])

    def test_no_sms_read_restores_state_and_ignores_late_modem_commands(self):
        controller = self.controller()
        self.observe(controller, 0)
        self.wait_for_log(controller, "POWER OFF")
        controller.request(b"+")
        controller.expect(self, CMTI, auto_reply=0)
        controller.write(b"AT+CMGR=1\x00x\r")
        self.assertIsNone(controller.read(0.4))
        controller.expect(self, frame(b"I30001")[:-1], timeout=4)
        controller.write(b"AT+CMGR=1\rAT+CMGD=1\r")
        controller.write(frame(b"J30001", 0))
        self.wait_for_log(controller, "timed out waiting for AT+CMGR")
        self.assert_only_polls(controller, 0)
        self.assertEqual(self.switch_states(controller), ["OFF", "ON", "OFF"])

    def test_late_or_unrelated_frames_cannot_confirm_a_request(self):
        controller = self.controller()
        self.observe(controller, 0)
        self.wait_for_log(controller, "POWER OFF")
        controller.request(b"+")
        self.exchange(controller, b"Start", 0, delete=False)
        controller.expect(self, frame(b"I30001")[:-1])
        controller.write(frame(b"J30005", 5))
        next_poll = controller.read(0.8)  # The verification read has timed out.
        self.assertEqual(next_poll, frame(b"I30005")[:-1])
        controller.write(frame(b"J30001", 1))
        controller.reply_to_query(next_poll, 0)
        self.observe(controller, 0)
        self.assertNotIn("Power direction confirmed", controller.logs())
        self.assertEqual(self.switch_states(controller), ["OFF", "ON"])
        self.observe(controller, 1)
        self.wait_for_log(controller, "Power direction confirmed")

    def test_alarm_ends_verification_and_blocks_power(self):
        controller = self.controller()
        self.observe(controller, 0)
        self.wait_for_log(controller, "POWER OFF")
        controller.request(b"+")
        self.exchange(controller, b"Start", 0)
        self.observe(controller, 8)
        self.wait_for_log(controller, "Power verification ended in Safety or Block")
        self.wait_for_log(controller, "POWER OFF", count=2)
        for state in (8, 9):
            self.observe(controller, state)
            controller.request(b"+-")
            self.assert_only_polls(controller, state)
        self.assertNotIn("Power direction confirmed", controller.logs())

    def test_unknown_and_undocumented_states_block_power(self):
        controller = self.controller()
        for state in (99, 29, 12, 99):
            self.observe(controller, state)
            controller.request(b"+-")
            self.assert_only_polls(controller, state, duration=0.24)
        self.wait_for_log(controller, "Italian Normale")
        self.wait_for_log(controller, "English ON")
        self.wait_for_log(controller, "UNCERTAIN YES")
        self.assertEqual(self.switch_states(controller), [])

    def test_lost_link_cancels_queued_request_and_requires_fresh_state(self):
        controller = self.controller()
        self.observe(controller, 0)
        self.wait_for_log(controller, "POWER OFF")
        controller.expect(self, frame(b"I30005")[:-1], auto_reply=0)
        controller.request(b"+")
        self.wait_for_log(controller, "POWER ON")
        self.wait_for_log(controller, "OFFLINE YES", count=2)
        self.wait_for_log(controller, "cancelled before sending")
        controller.request(b"++--")
        self.observe(controller, 5)
        self.assert_only_polls(controller, 5)
        self.assertEqual(self.switch_states(controller), ["OFF", "ON"])

    def test_fresh_read_can_cancel_a_request_after_panel_change(self):
        controller = self.controller()
        self.observe(controller, 0)
        self.wait_for_log(controller, "POWER OFF")
        controller.request(b"+")
        self.wait_for_log(controller, "POWER ON")
        self.observe(controller, 7, previous=0)
        self.wait_for_log(controller, "cancelled before sending")
        self.assert_only_polls(controller, 7)
        self.assertEqual(self.switch_states(controller), ["OFF", "ON", "OFF"])

    def test_lost_link_after_sms_preserves_uncertain_intent(self):
        controller = self.controller()
        self.observe(controller, 5)
        self.wait_for_log(controller, "POWER ON")
        controller.request(b"-")
        self.exchange(controller, b"Stop", 5)
        controller.expect(self, frame(b"I30001")[:-1])
        self.wait_for_log(controller, "OFFLINE YES", count=2)
        controller.request(b"++--")
        self.observe(controller, 5)
        self.assert_only_polls(controller, 5)
        self.assertEqual(self.switch_states(controller), ["ON", "OFF"])
        self.assertNotIn("Power direction confirmed", controller.logs())
        self.observe(controller, 7)
        self.wait_for_log(controller, "Power direction confirmed")

    def test_default_switch_uses_original_frames_and_parser(self):
        controller = self.controller("standard")
        self.observe(controller, 5)
        self.wait_for_log(controller, "POWER ON")
        controller.request(b"+w")
        controller.expect(self, frame(b"J30253", 1)[:-1], auto_reply=5)
        controller.write(frame(b"J30253", 1))
        controller.expect(self, frame(b"B20364", 1)[:-1], auto_reply=5)
        controller.write(frame(b"B20364", 1))
        controller.request(b"-")
        controller.expect(self, frame(b"J30254", 1)[:-1], auto_reply=5)

    def test_yaml_migration(self):
        baseline = (self.directory / "fourheat-standard.yaml").read_text()
        config = self.directory / "yaml-check.yaml"
        point = "    datapoint: J30001\n"
        cases = (
            ("    legacy_power_control: true\n", True, "deprecated"),
            ("    legacy_power_control: false\n", True, "deprecated"),
            ("    power_control: gsm\n    legacy_power_control: true\n", True, "deprecated"),
            ("    power_control: standard\n", True, None),
            ("    power_control: standard\n    legacy_power_control: true\n", False, "conflicts"),
            ("    power_control: gsm\n    legacy_power_control: false\n", False, "conflicts"),
            ("    power_control: gsm\n    query_datapoint: I30002\n", False, "uses query I30001"),
        )
        for switch_config, valid, diagnostic in cases:
            with self.subTest(switch_config=switch_config):
                config.write_text(baseline.replace(point, point + switch_config, 1))
                self.validate_yaml(config, valid, diagnostic)
        config.write_text(baseline.replace(point, "", 1))
        self.validate_yaml(config, False, "datapoint is required")

    def validate_yaml(self, config, valid, diagnostic):
        result = subprocess.run(["esphome", "config", config], capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, valid, result.stdout + result.stderr)
        if diagnostic:
            self.assertIn(diagnostic, result.stdout + result.stderr)

    def switch_states(self, controller):
        return re.findall(r"\bPOWER (ON|OFF)\b", controller.logs())

    def wait_for_log(self, controller, message, count=1):
        deadline = time.monotonic() + 3
        while controller.logs().count(message) < count:
            self.assertLess(time.monotonic(), deadline, controller.logs())
            time.sleep(0.01)


class PowerPolicyTest(unittest.TestCase):
    def test_timers_and_policy(self):
        component = ROOT / "components" / "fourheat"
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "gsm_power_policy"
            subprocess.run(
                [
                    os.environ.get("CXX", "g++"),
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    component,
                    ROOT / "tests" / "gsm_power_policy.cpp",
                    component / "gsm_modem.cpp",
                    component / "gsm_power.cpp",
                    component / "operating_state.cpp",
                    "-o",
                    binary,
                ],
                check=True,
            )
            subprocess.run([binary], check=True)


if __name__ == "__main__":
    unittest.main()
