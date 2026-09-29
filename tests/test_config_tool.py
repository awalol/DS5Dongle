import io
import unittest
from contextlib import redirect_stdout
from types import SimpleNamespace
from unittest.mock import patch

from tools import config_tool


class FakeDevice:
    def __init__(self):
        self.reports = []
        self.closed = False

    def send_feature_report(self, report):
        self.reports.append(bytes(report))

    def close(self):
        self.closed = True


class ConfigToolPollingTests(unittest.TestCase):
    def test_mode_three_is_valid_and_set_prints_reconnect_action(self):
        self.assertEqual(config_tool.parse_assignment("polling_rate_mode=3"), ("polling_rate_mode", 3))
        with self.assertRaises(SystemExit):
            config_tool.parse_assignment("polling_rate_mode=4")

        output = io.StringIO()
        fake = FakeDevice()
        with (
            patch.object(config_tool, "open_device", return_value=fake),
            patch.object(config_tool, "read_config", return_value={"polling_rate_mode": 1}),
            patch.object(config_tool, "read_config_snapshot", return_value=({"polling_rate_mode": 3}, None)),
            patch.object(config_tool, "write_config"),
            redirect_stdout(output),
        ):
            config_tool.cmd_set(SimpleNamespace(assignments=["polling_rate_mode=3"], no_save=True))

        self.assertIn("reconnect", output.getvalue().lower())
        self.assertIn("replug", output.getvalue().lower())
        self.assertTrue(fake.closed)

    def test_explicit_reconnect_command_uses_existing_f6_function_three(self):
        fake = FakeDevice()
        with patch.object(config_tool, "open_device", return_value=fake):
            config_tool.cmd_reconnect(SimpleNamespace())

        self.assertEqual(len(fake.reports), 1)
        self.assertEqual(len(fake.reports[0]), config_tool.FEATURE_REPORT_LEN)
        self.assertEqual(fake.reports[0][:2], bytes([config_tool.REPORT_SET, config_tool.FUNC_RECONNECT]))
        self.assertTrue(fake.closed)


class BatteryDevice(FakeDevice):
    def __init__(self, responses, write_results=None, repeat_last=False):
        super().__init__()
        self.responses = list(responses)
        self.write_results = list(write_results or [])
        self.repeat_last = repeat_last

    def get_feature_report(self, report_id, length):
        assert report_id == 0xF7
        assert length == 64
        if self.repeat_last and len(self.responses) == 1:
            return self.responses[0]
        return self.responses.pop(0)

    def send_feature_report(self, report):
        super().send_feature_report(report)
        return self.write_results.pop(0) if self.write_results else len(report)


def battery_response(enabled=False, status=0, *, report_id=True, marker=b"BF", version=1):
    body = bytes((5,)) + bytes(21)
    tail = marker + bytes((version, int(enabled), status))
    return (bytes((0xF7,)) if report_id else b"") + body + tail


def base_response(volume, *, pin=255, save_status=0, extension=True):
    # Literal v5/22 body: version, unity gain, volumes, remaining validated fields.
    body = bytes((5, 0, 0, 128, 63, volume, 100, 2, 30, 0, 1, 48,
                  2, 0, 0, 0, 0, 0, 0, 0, pin, 0))
    tail = b"BF\x01\x00" + bytes((save_status,)) if extension else bytes(41)
    return bytes((0xF7,)) + body + tail


class FakeClock:
    def __init__(self):
        self.now = 0.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class BaseConfigSaveTests(unittest.TestCase):
    def test_empty_f7_snapshot_reports_read_error(self):
        fake = BatteryDevice([None])
        with self.assertRaisesRegex(SystemExit, "Empty response reading config"):
            config_tool.read_config_snapshot(fake)

    def test_snapshot_uses_one_f7_for_base_and_save_status(self):
        fake = BatteryDevice([
            base_response(90, save_status=0),
            base_response(91, save_status=1),
        ])
        cfg, extension = config_tool.read_config_snapshot(fake)
        self.assertEqual(cfg["speaker_volume"], 90)
        self.assertEqual(extension.save_status, 0)
        self.assertEqual(fake.responses, [base_response(91, save_status=1)])

    def test_mixed_f7_responses_cannot_falsely_verify_flash_save(self):
        fake = BatteryDevice([
            base_response(100),
            base_response(90, save_status=0),
            base_response(91, save_status=0),
            base_response(90, save_status=0),
            base_response(91, save_status=1),
        ])
        with (patch.object(config_tool, "open_device", return_value=fake),
              patch.object(config_tool, "READBACK_TIMEOUT_S", 0)):
            with self.assertRaisesRegex(SystemExit, "save verification failed"):
                config_tool.cmd_set(SimpleNamespace(assignments=["speaker_volume=90"], no_save=False))
        self.assertEqual([report[1] for report in fake.reports], [1, 2])

    def test_reserved_pin_is_reported_as_clamped_after_observed_change(self):
        fake = BatteryDevice([
            base_response(100, pin=4),
            base_response(100, pin=255, save_status=0),
        ])
        output = io.StringIO()
        with (patch.object(config_tool, "open_device", return_value=fake),
              redirect_stdout(output)):
            config_tool.cmd_set(SimpleNamespace(assignments=["status_gpio_pin=254"], no_save=True))
        self.assertIn("status_gpio_pin -> 255", output.getvalue())
        self.assertIn("clamped", output.getvalue().lower())

    def test_prior_disabled_pin_stale_first_f7_does_not_hide_valid_new_pin(self):
        fake = BatteryDevice([
            base_response(100, pin=255),
            base_response(100, pin=255, save_status=0),
            base_response(100, pin=5, save_status=0),
        ])
        clock = FakeClock()
        output = io.StringIO()
        with (patch.object(config_tool, "open_device", return_value=fake),
              patch.object(config_tool, "time", clock, create=True),
              redirect_stdout(output)):
            config_tool.cmd_set(SimpleNamespace(assignments=["status_gpio_pin=5"], no_save=True))
        self.assertIn("status_gpio_pin -> 5", output.getvalue())
        self.assertNotIn("clamped", output.getvalue().lower())
        self.assertGreater(clock.now, 0)

    def test_prior_disabled_pin_reserved_request_waits_before_clamp_result(self):
        fake = BatteryDevice([
            base_response(100, pin=255), base_response(100, pin=255)
        ], repeat_last=True)
        clock = FakeClock()
        output = io.StringIO()
        with (patch.object(config_tool, "open_device", return_value=fake),
              patch.object(config_tool, "time", clock, create=True),
              patch.object(config_tool, "READBACK_TIMEOUT_S", 0.06, create=True),
              redirect_stdout(output)):
            config_tool.cmd_set(SimpleNamespace(assignments=["status_gpio_pin=254"], no_save=True))
        self.assertIn("status_gpio_pin -> 255", output.getvalue())
        self.assertIn("clamped", output.getvalue().lower())
        self.assertGreaterEqual(clock.now, 0.06)
        self.assertIn("update unconfirmed", output.getvalue().lower())

    def test_unchanged_disabled_pin_save_reports_verified_flash_and_unconfirmed_update(self):
        fake = BatteryDevice([
            base_response(100, pin=255),
            base_response(100, pin=255, save_status=0),
            base_response(100, pin=255, save_status=1),
        ])
        clock = FakeClock()
        output = io.StringIO()
        with (patch.object(config_tool, "open_device", return_value=fake),
              patch.object(config_tool, "time", clock, create=True),
              patch.object(config_tool, "READBACK_TIMEOUT_S", 0, create=True),
              redirect_stdout(output)):
            config_tool.cmd_set(SimpleNamespace(assignments=["status_gpio_pin=254"], no_save=False))
        self.assertIn("flash save verified", output.getvalue().lower())
        self.assertIn("update unconfirmed", output.getvalue().lower())
        self.assertEqual([report[1] for report in fake.reports], [1, 2])

    def test_stale_base_readback_retries_until_update_and_save_are_visible(self):
        fake = BatteryDevice([
            base_response(100),
            base_response(100, save_status=1),
            base_response(90, save_status=0),
            base_response(90, save_status=0),
            base_response(90, save_status=1),
        ])
        clock = FakeClock()
        with (patch.object(config_tool, "open_device", return_value=fake),
              patch.object(config_tool, "time", clock, create=True)):
            config_tool.cmd_set(SimpleNamespace(assignments=["speaker_volume=90"], no_save=False))
        self.assertEqual([report[1] for report in fake.reports], [1, 2])
        self.assertGreater(clock.now, 0)

    def test_permanent_base_mismatch_times_out_without_saving(self):
        fake = BatteryDevice([base_response(100), base_response(100)], repeat_last=True)
        clock = FakeClock()
        with (patch.object(config_tool, "open_device", return_value=fake),
              patch.object(config_tool, "time", clock, create=True),
              patch.object(config_tool, "READBACK_TIMEOUT_S", 0.06, create=True)):
            with self.assertRaisesRegex(SystemExit, "Config RAM update readback"):
                config_tool.cmd_set(SimpleNamespace(assignments=["speaker_volume=90"], no_save=False))
        self.assertEqual([report[1] for report in fake.reports], [1])
        self.assertGreaterEqual(clock.now, 0.06)

    def test_capable_firmware_failed_flash_status_is_not_reported_saved(self):
        fake = BatteryDevice([
            base_response(100), base_response(90, save_status=0),
            base_response(90, save_status=2),
        ])
        with (patch.object(config_tool, "open_device", return_value=fake),
              patch.object(config_tool, "READBACK_TIMEOUT_S", 0)):
            with self.assertRaisesRegex(SystemExit, "save verification failed"):
                config_tool.cmd_set(SimpleNamespace(assignments=["speaker_volume=90"], no_save=False))
        self.assertEqual([report[1] for report in fake.reports], [1, 2])

    def test_capable_firmware_requires_base_readback_after_save(self):
        fake = BatteryDevice([
            base_response(100), base_response(90, save_status=0),
            base_response(91, save_status=1),
        ])
        with (patch.object(config_tool, "open_device", return_value=fake),
              patch.object(config_tool, "READBACK_TIMEOUT_S", 0)):
            with self.assertRaisesRegex(SystemExit, "base readback"):
                config_tool.cmd_set(SimpleNamespace(assignments=["speaker_volume=90"], no_save=False))

    def test_capable_firmware_reports_verified_only_after_both_readbacks(self):
        fake = BatteryDevice([
            base_response(100), base_response(90, save_status=0),
            base_response(90, save_status=1),
        ])
        output = io.StringIO()
        with patch.object(config_tool, "open_device", return_value=fake), redirect_stdout(output):
            config_tool.cmd_set(SimpleNamespace(assignments=["speaker_volume=90"], no_save=False))
        self.assertIn("verified", output.getvalue().lower())
        self.assertEqual([report[1] for report in fake.reports], [1, 2])

    def test_legacy_firmware_labels_save_unverified(self):
        fake = BatteryDevice([
            base_response(100, extension=False),
            base_response(90, extension=False),
            base_response(90, extension=False),
        ])
        output = io.StringIO()
        with patch.object(config_tool, "open_device", return_value=fake), redirect_stdout(output):
            config_tool.cmd_set(SimpleNamespace(assignments=["speaker_volume=90"], no_save=False))
        self.assertIn("unverified", output.getvalue().lower())
        self.assertNotIn("saved to flash", output.getvalue().lower())

    def test_short_update_send_stops_before_save(self):
        fake = BatteryDevice([base_response(100)], write_results=[0])
        with patch.object(config_tool, "open_device", return_value=fake):
            with self.assertRaisesRegex(SystemExit, "feature report write incomplete"):
                config_tool.cmd_set(SimpleNamespace(assignments=["speaker_volume=90"], no_save=False))
        self.assertEqual([report[1] for report in fake.reports], [1])

    def test_short_save_send_stops_before_saved_claim(self):
        fake = BatteryDevice([
            base_response(100), base_response(90, save_status=0)
        ], write_results=[64, 0])
        with patch.object(config_tool, "open_device", return_value=fake):
            with self.assertRaisesRegex(SystemExit, "feature report write incomplete"):
                config_tool.cmd_set(SimpleNamespace(assignments=["speaker_volume=90"], no_save=False))
        self.assertEqual([report[1] for report in fake.reports], [1, 2])


class BatteryFeedbackTests(unittest.TestCase):
    def test_stale_first_f7_retries_ram_and_flash_readbacks(self):
        fake = BatteryDevice([
            battery_response(False, 0),
            battery_response(False, 0), battery_response(True, 0),
            battery_response(True, 0), battery_response(True, 1),
        ])
        clock = FakeClock()
        with (patch.object(config_tool, "open_device", return_value=fake),
              patch.object(config_tool, "time", clock, create=True)):
            config_tool.cmd_battery_feedback(SimpleNamespace(action="on", no_save=False))
        self.assertEqual(len(fake.reports), 2)
        self.assertGreater(clock.now, 0)

    def test_permanent_ram_mismatch_times_out_without_saving(self):
        fake = BatteryDevice([battery_response(False, 0), battery_response(False, 0)],
                             repeat_last=True)
        clock = FakeClock()
        with (patch.object(config_tool, "open_device", return_value=fake),
              patch.object(config_tool, "time", clock, create=True),
              patch.object(config_tool, "READBACK_TIMEOUT_S", 0.06, create=True)):
            with self.assertRaisesRegex(SystemExit, "Battery feedback readback"):
                config_tool.cmd_battery_feedback(SimpleNamespace(action="on", no_save=False))
        self.assertEqual(len(fake.reports), 1)
        self.assertGreaterEqual(clock.now, 0.06)

    def test_permanent_flash_status_mismatch_times_out(self):
        fake = BatteryDevice([
            battery_response(False, 0), battery_response(True, 0), battery_response(True, 0)
        ], repeat_last=True)
        clock = FakeClock()
        with (patch.object(config_tool, "open_device", return_value=fake),
              patch.object(config_tool, "time", clock, create=True),
              patch.object(config_tool, "READBACK_TIMEOUT_S", 0.06, create=True)):
            with self.assertRaisesRegex(SystemExit, "save verification failed"):
                config_tool.cmd_battery_feedback(SimpleNamespace(action="on", no_save=False))
        self.assertEqual(len(fake.reports), 2)
        self.assertGreaterEqual(clock.now, 0.06)

    def test_get_reads_marked_tail_after_original_body(self):
        fake = BatteryDevice([battery_response()])
        output = io.StringIO()
        with patch.object(config_tool, "open_device", return_value=fake), redirect_stdout(output):
            config_tool.cmd_battery_feedback(SimpleNamespace(action="get", no_save=False))
        self.assertIn("off", output.getvalue().lower())
        self.assertEqual(fake.reports, [])
        self.assertTrue(fake.closed)

    def test_on_writes_only_new_function_and_requires_verified_save(self):
        fake = BatteryDevice([
            battery_response(False, 0),
            battery_response(True, 0),
            battery_response(True, 1),
        ])
        output = io.StringIO()
        with patch.object(config_tool, "open_device", return_value=fake), redirect_stdout(output):
            config_tool.cmd_battery_feedback(SimpleNamespace(action="on", no_save=False))
        self.assertEqual(fake.reports, [
            bytes((0xF6, 0x05, 0x42, 0x46, 0x01, 0x01)) + bytes(58),
            bytes((0xF6, 0x02)) + bytes(62),
        ])
        self.assertIn("verified", output.getvalue().lower())
        self.assertTrue(fake.closed)

    def test_no_save_confirms_ram_without_sending_save(self):
        fake = BatteryDevice([battery_response(False), battery_response(True)])
        output = io.StringIO()
        with patch.object(config_tool, "open_device", return_value=fake), redirect_stdout(output):
            config_tool.cmd_battery_feedback(SimpleNamespace(action="on", no_save=True))
        self.assertEqual(len(fake.reports), 1)
        self.assertEqual(fake.reports[0][:6], bytes((0xF6, 0x05, 0x42, 0x46, 0x01, 0x01)))
        self.assertIn("ram", output.getvalue().lower())
        self.assertNotIn("verified", output.getvalue().lower())

    def test_old_firmware_does_not_receive_new_command(self):
        fake = BatteryDevice([bytes((0xF7, 5)) + bytes(21) + bytes(41)])
        with patch.object(config_tool, "open_device", return_value=fake):
            with self.assertRaisesRegex(SystemExit, "not supported"):
                config_tool.cmd_battery_feedback(SimpleNamespace(action="on", no_save=False))
        self.assertEqual(fake.reports, [])
        self.assertTrue(fake.closed)

    def test_readback_mismatch_prevents_save(self):
        fake = BatteryDevice([battery_response(False), battery_response(False)])
        with (patch.object(config_tool, "open_device", return_value=fake),
              patch.object(config_tool, "READBACK_TIMEOUT_S", 0)):
            with self.assertRaisesRegex(SystemExit, "readback"):
                config_tool.cmd_battery_feedback(SimpleNamespace(action="on", no_save=False))
        self.assertEqual(len(fake.reports), 1)

    def test_failed_flash_status_cannot_be_called_saved(self):
        fake = BatteryDevice([
            battery_response(False), battery_response(True), battery_response(True, 2)
        ])
        with (patch.object(config_tool, "open_device", return_value=fake),
              patch.object(config_tool, "READBACK_TIMEOUT_S", 0)):
            with self.assertRaisesRegex(SystemExit, "save verification"):
                config_tool.cmd_battery_feedback(SimpleNamespace(action="on", no_save=False))
        self.assertEqual(len(fake.reports), 2)

    def test_existing_on_state_does_not_mask_short_update_write(self):
        fake = BatteryDevice([battery_response(True, 1)], write_results=[0])
        with patch.object(config_tool, "open_device", return_value=fake):
            with self.assertRaisesRegex(SystemExit, "feature report write incomplete"):
                config_tool.cmd_battery_feedback(SimpleNamespace(action="on", no_save=False))
        self.assertEqual(len(fake.reports), 1)

    def test_existing_verified_status_does_not_mask_short_save_write(self):
        fake = BatteryDevice([battery_response(True, 1), battery_response(True, 0)],
                             write_results=[64, 0])
        with patch.object(config_tool, "open_device", return_value=fake):
            with self.assertRaisesRegex(SystemExit, "feature report write incomplete"):
                config_tool.cmd_battery_feedback(SimpleNamespace(action="on", no_save=False))
        self.assertEqual(len(fake.reports), 2)

    def test_idempotent_update_must_clear_prior_verified_status(self):
        fake = BatteryDevice([battery_response(True, 1), battery_response(True, 1)])
        with (patch.object(config_tool, "open_device", return_value=fake),
              patch.object(config_tool, "READBACK_TIMEOUT_S", 0)):
            with self.assertRaisesRegex(SystemExit, "readback"):
                config_tool.cmd_battery_feedback(SimpleNamespace(action="on", no_save=False))
        self.assertEqual(len(fake.reports), 1)

    def test_invalid_or_short_base_does_not_advertise_battery_capability(self):
        for response in (
            bytes((0xF7, 4)) + bytes(21) + b"BF\x01\x01\x00",
            bytes((0xF7, 5)) + bytes(10),
        ):
            with self.subTest(response=response):
                fake = BatteryDevice([response])
                with self.assertRaisesRegex(SystemExit, "Invalid F7 base config"):
                    config_tool.read_battery_extension(fake)

    def test_tail_without_report_id_is_detected_at_body_offset(self):
        fake = BatteryDevice([battery_response(True, 1, report_id=False)])
        with patch.object(config_tool, "open_device", return_value=fake):
            extension = config_tool.read_battery_extension(fake)
        self.assertTrue(extension.enabled)
        self.assertEqual(extension.save_status, 1)

    def test_invalid_marked_extension_is_rejected(self):
        for response in (
            battery_response(marker=b"BF", version=2),
            bytes((0xF7, 5)) + bytes(21) + b"BF\x01\x02\x00",
            bytes((0xF7, 5)) + bytes(21) + b"BF\x01\x00\x03",
        ):
            with self.subTest(response=response):
                fake = BatteryDevice([response])
                with self.assertRaisesRegex(SystemExit, "Invalid battery extension"):
                    config_tool.read_battery_extension(fake)

    def test_single_marker_byte_is_truncated_extension(self):
        fake = BatteryDevice([bytes((0xF7, 5)) + bytes(21) + b"B"])
        with self.assertRaisesRegex(SystemExit, "Invalid battery extension"):
            config_tool.read_battery_extension(fake)

    def test_b_prefixed_padded_tail_is_malformed_not_legacy(self):
        for tail in (b"B\x00", b"B\x00" + bytes(39)):
            with self.subTest(tail=tail):
                fake = BatteryDevice([bytes((0xF7, 5)) + bytes(21) + tail])
                with self.assertRaisesRegex(SystemExit, "Invalid battery extension"):
                    config_tool.read_battery_extension(fake)


if __name__ == "__main__":
    unittest.main()
