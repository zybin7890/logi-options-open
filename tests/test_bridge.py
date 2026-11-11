"""No hardware/process starts: API consent failures and platform dispatch."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("bridge", Path(__file__).resolve().parents[1] / "bridge.py")
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


class BridgeTests(unittest.TestCase):
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
