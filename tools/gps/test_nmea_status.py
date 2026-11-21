import unittest

from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

spec = spec_from_file_location("nmea_status", Path(__file__).with_name("nmea-status.py"))
module = module_from_spec(spec)
spec.loader.exec_module(module)


def sentence(body):
    data = body.encode("ascii")
    checksum = 0
    for byte in data:
        checksum ^= byte
    return b"$" + data + f"*{checksum:02X}\r\n".encode()


class SignalStatusTests(unittest.TestCase):
    def test_predicted_satellites_do_not_count_as_received_signals(self):
        status = module.summarize(sentence("GPGSV,1,1,02,01,45,120,,02,30,200,00"))
        self.assertEqual(status["satellites_reported_max"], 2)
        self.assertEqual(status["signal_samples"], 0)
        self.assertEqual(status["gga_valid"], 0)

    def test_multiconstellation_signal_id_and_empty_cn0(self):
        data = sentence("GAGSV,1,1,02,11,45,120,35,12,30,200,,7")
        data += sentence("GLGSV,1,1,01,65,20,100,42")
        status = module.summarize(data)
        self.assertEqual(status["signal_samples"], 2)
        self.assertEqual(status["cn0_max_dbhz"], 42)
        self.assertEqual(status["malformed"], 0)

    def test_invalid_cn0_and_bad_checksum_cannot_count(self):
        data = sentence("GPGSV,1,1,01,01,45,120,nan")
        data += sentence("GPGSV,1,1,01,01,45,120,100")
        data += b"$GPGSV,1,1,01,01,45,120,45*00\r\n"
        status = module.summarize(data)
        self.assertEqual(status["malformed"], 2)
        self.assertEqual(status["checksum_errors"], 1)
        self.assertEqual(status["signal_samples"], 0)


if __name__ == "__main__":
    unittest.main()
