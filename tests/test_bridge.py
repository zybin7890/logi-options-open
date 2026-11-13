"""No hardware/process starts: API consent failures and platform dispatch."""
import importlib.util
from pathlib import Path
import unittest
import tempfile
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("bridge", Path(__file__).resolve().parents[1] / "bridge.py")
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


class BridgeTests(unittest.TestCase):
    def save_with_reply(self, reply, windows=True):
        def cli(command, *_args, **_kwargs):
            if command == "reload":
                if isinstance(reply, Exception):
                    raise reply
                return reply
            return "valid"

        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "mappings.ini"
            with patch.object(bridge, "WINDOWS", windows), patch.object(bridge, "CONFIG", config), \
                    patch.object(bridge, "cli", side_effect=cli):
                result = bridge.write_config({"text": "[bindings]\n"})
            self.assertEqual(config.read_text(), "[bindings]\n")
            return result

    def test_windows_reload_exposes_applied_ack(self):
        ack = {"confirmed": True, "status": "applied", "request_id": 7,
               "instance_id": 9, "pid": 123, "revision": 1, "fingerprint": "a" * 64}
        result = self.save_with_reply(ack)
        self.assertTrue(result["saved"])
        self.assertTrue(result["reload_requested"])
        self.assertTrue(result["reloaded"])
        for key, value in ack.items():
            self.assertEqual(result[key], value)

    def test_windows_unconfirmed_or_inconsistent_ack_is_not_success(self):
        for ack in ({"confirmed": False, "status": "timeout"},
                    {"confirmed": True, "status": "superseded"},
                    {"confirmed": "true", "status": "applied"}):
            with self.subTest(ack=ack):
                result = self.save_with_reply(ack)
                self.assertTrue(result["saved"])
                self.assertFalse(result["reload_requested"])
                self.assertIn("reload_error", result)

    def test_reload_failure_preserves_saved_configuration(self):
        result = self.save_with_reply(bridge.BridgeError("Backend unavailable", 503))
        self.assertEqual(result, {"saved": True, "reload_requested": False,
                                  "reload_error": "Backend unavailable"})

    def test_legacy_windows_request_is_not_an_applied_ack(self):
        self.assertEqual(self.save_with_reply({"reload_requested": True}),
                         {"saved": True, "reload_requested": True})

    def test_linux_confirmation_contract_is_preserved(self):
        self.assertEqual(self.save_with_reply({"reload_requested": True, "reloaded": True}, False),
                         {"saved": True, "reload_requested": True, "reloaded": True})
        result = self.save_with_reply({"reload_requested": True, "reloaded": False}, False)
        self.assertIn("reload_error", result)

    def test_preview_requires_consent_before_start(self):
        def cli(command, *_args, **_kwargs):
            if command == "check-disclaimer":
                raise bridge.BridgeError("First-use notice not acknowledged", 409)
            return "valid"

        with patch.object(bridge, "WINDOWS", True), patch.object(bridge, "cli", side_effect=cli), \
                patch.object(bridge.subprocess, "Popen") as spawn:
            with self.assertRaises(bridge.BridgeError):
                bridge.preview_actions({"sequence": "Ctrl+C"})
            spawn.assert_not_called()

    def test_linux_start_uses_run_even_when_takeover_requested(self):
        def cli(command, *_args, **_kwargs):
            return {"native": False} if command == "agent-status" else "valid"

        with patch.object(bridge, "WINDOWS", False), patch.object(bridge, "cli", side_effect=cli), \
                patch.object(bridge, "launch", return_value={"pid": 1}) as launch:
            self.assertEqual(bridge.perform("start", {"takeover": True}), {"pid": 1})
            launch.assert_called_once_with("run", bridge.CONFIG, log=True, wait_ready=True)

    def test_unacknowledged_start_never_launches(self):
        def cli(command, *_args, **_kwargs):
            if command == "check-disclaimer":
                raise bridge.BridgeError("First-use notice not acknowledged", 409)
            return {"native": False} if command == "agent-status" else "valid"

        with patch.object(bridge, "cli", side_effect=cli), patch.object(bridge, "launch") as launch:
            with self.assertRaises(bridge.BridgeError):
                bridge.perform("start", {"takeover": True})
            launch.assert_not_called()


if __name__ == "__main__":
    unittest.main()
