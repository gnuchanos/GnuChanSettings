import importlib.util
import unittest
from pathlib import Path
from importlib.machinery import SourceFileLoader


HELPER_PATH = Path(__file__).resolve().parents[1] / "gnuchan-settings-helper"
LOADER = SourceFileLoader("gnuchan_settings_helper", str(HELPER_PATH))
SPEC = importlib.util.spec_from_loader("gnuchan_settings_helper", LOADER)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("could not load the USB settings helper")
HELPER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HELPER)


class UsbHelperValidationTests(unittest.TestCase):
    def test_rejects_path_like_device_identity(self):
        for identity in ("../1-2", ".", "..", "1-2:1.0",
                         "/sys/devices/1-2"):
            with self.subTest(identity=identity):
                with self.assertRaisesRegex(ValueError, "identity"):
                    HELPER.apply_usb_power(identity, "on")

    def test_accepts_only_supported_power_modes(self):
        with self.assertRaisesRegex(ValueError, "power mode"):
            HELPER.apply_usb_power("1-2.3", "suspend")


if __name__ == "__main__":
    unittest.main()
