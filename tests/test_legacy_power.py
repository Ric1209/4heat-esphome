"""Exercise the real ESPHome host component against a simulated RS232 peer."""

import os
from pathlib import Path
import pty
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
    def __init__(self, program, port):
        self.pty, self.slave = pty.openpty()
        tty.setraw(self.slave)
        port.unlink(missing_ok=True)
        port.symlink_to(os.ttyname(self.slave))
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen(
            [program], stdout=self.log, stderr=subprocess.STDOUT
        )
        self.buffer = b""

    def close(self):
        self.process.terminate()
        self.process.wait(timeout=5)
        os.close(self.pty)
        os.close(self.slave)
        self.log.close()

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


class LegacyPowerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(temp.cleanup)
        directory = Path(temp.name)
        cls.port = Path("/tmp") / f"{directory.name}-fourheat-serial"
        cls.addClassCleanup(cls.port.unlink, missing_ok=True)
        cls.programs = {}
        for mode, option in (("legacy", "    legacy_power_control: true"), ("default", "")):
            name = f"fourheat-{mode}"
            build = directory / name
            yaml = directory / f"{name}.yaml"
            yaml.write_text(f"""
esphome:
  name: {name}
  build_path: {build}
  on_boot:
    priority: -100
    then:
      - delay: 700ms
      - switch.turn_on: heater
      - delay: 500ms
      - lambda: id(bus).send_string_value("B20364", "1");
      - delay: 3800ms
      - switch.turn_off: heater
host:
logger:
  level: DEBUG
external_components:
  - source:
      type: local
      path: {ROOT / 'components'}
uart:
  port: {cls.port}
  baud_rate: 9600
fourheat:
  id: bus
  update_interval: 200ms
switch:
  - platform: fourheat
    id: heater
    name: Heater
    restore_mode: DISABLED
    datapoint: J30001
    on_datapoint: J30253
    off_datapoint: J30254
    on_data: '1'
    off_data: '1'
{option}
sensor:
  - platform: fourheat
    name: Exhaust
    datapoint: J30005
""")
            subprocess.run(["esphome", "compile", yaml], check=True)
            cls.programs[mode] = build / ".pioenvs" / name / "program"

    def controller(self, mode):
        controller = SimulatedController(self.programs[mode], self.port)
        self.addCleanup(controller.close)
        return controller

    def test_start_and_stop_follow_fresh_state(self):
        controller = self.controller("legacy")
        controller.expect(self, CMTI, auto_reply=0)
        controller.write(b"AT+CMGR=2\rAT+CMGR=\x001\rAT+CM")
        # The unrelated queued write becomes due while the GSM exchange owns UART.
        self.assertIsNone(controller.read(0.65))
        controller.write(b"GR=1\x00\x00\r\n")
        for line in (SMS, b"Start", b"OK"):
            controller.expect(self, line)
        controller.write(b"AT+CMGR=1\rAT+UNSUPPORTED\r")
        controller.expect(self, frame(b"I30001")[:-1])
        controller.write(frame(b"J30005", 5))
        controller.write(frame(b"J30001", 2))
        controller.expect(self, frame(b"B20364", 1)[:-1], auto_reply=2)
        controller.write(frame(b"B20364", 1))
        controller.expect(self, CMTI, timeout=6, auto_reply=5)
        controller.write(b"AT+CMGR=1\r")
        for line in (SMS, b"Stop", b"OK"):
            controller.expect(self, line)
        controller.expect(self, frame(b"I30001")[:-1])
        controller.write(frame(b"J30001", 7))
        self.wait_for_log(controller, "shutdown in progress")
        deadline = time.monotonic() + 4
        while "target observed in J30001: off" not in controller.logs():
            self.assertLess(time.monotonic(), deadline, controller.logs())
            line = controller.read(0.3)
            if line:
                self.assertTrue(line.startswith(b"\x08I"), line)
                controller.reply_to_query(line, 0)
        self.assertIn("startup in progress", controller.logs())
        self.assertIn("target observed in J30001: running", controller.logs())

    def test_timeout_releases_queue_without_replaying_power(self):
        controller = self.controller("legacy")
        controller.expect(self, CMTI, auto_reply=0)
        controller.write(b"AT+CMGR=1)\r")
        self.assertIsNone(controller.read(0.7))
        controller.expect(self, frame(b"B20364", 1)[:-1])
        controller.write(frame(b"B20364", 1))
        controller.expect(self, frame(b"I30005")[:-1], auto_reply=0)
        self.assertIn("timed out waiting for AT+CMGR", controller.logs())
        self.assertNotIn("target observed", controller.logs())

    def test_alarm_and_late_modem_line_are_not_confirmation(self):
        controller = self.controller("legacy")
        controller.expect(self, CMTI, auto_reply=0)
        controller.write(b"AT+CMGR=1\r")
        for line in (SMS, b"Start", b"OK"):
            controller.expect(self, line)
        controller.expect(self, frame(b"I30001")[:-1])
        controller.write(b"\r\nAT+CMGR=1\r")
        controller.write(frame(b"J30001", 9))
        self.wait_for_log(controller, "ended in Safety or Block")
        self.assertNotIn("target observed", controller.logs())

    def test_default_switch_uses_original_frames(self):
        controller = self.controller("default")
        controller.expect(self, frame(b"J30253", 1)[:-1], auto_reply=0)
        controller.write(frame(b"J30253", 1))
        controller.expect(self, frame(b"B20364", 1)[:-1], auto_reply=5)
        controller.write(frame(b"B20364", 1))
        controller.expect(self, frame(b"J30254", 1)[:-1], timeout=6, auto_reply=5)

    def wait_for_log(self, controller, message):
        deadline = time.monotonic() + 1
        while message not in controller.logs():
            self.assertLess(time.monotonic(), deadline, controller.logs())
            time.sleep(0.02)


if __name__ == "__main__":
    unittest.main()
