# SPDX-License-Identifier: MPL-2.0
import importlib.util
import pathlib
import struct
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("validate_capture", ROOT / "tools/validate_compute_capture.py")
validator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validator)

class CaptureValidation(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = pathlib.Path(self.temp.name)
        self.spirv = struct.pack("<5I", 0x07230203, 0x00010400, 0, 1, 0)
        (self.path / "shader.spv").write_bytes(self.spirv)
        (self.path / "guest.bin").write_bytes(b"guest")
        (self.path / "metadata.txt").write_text(
            "status=pending\nformat=1\nstage=compute\nguest_hash=0000000000000042\n"
            "guest_bytes=5\nspirv_bytes=20\napplication_api_version=4198400\n"
            "profile.supported_spirv=66560\n")
        (self.path / "result.txt").write_text("vk_result=-13\nvk_result_name=ErrorUnknown\n")

    def test_complete_failed_candidate_selects_vulkan11_spirv14(self):
        result = validator.inspect_capture(self.path)
        self.assertEqual(result["target_env"], "vulkan1.1spv1.4")
        self.assertEqual(result["vk_result"], "-13")
        self.assertEqual(result["guest_hash"], "0000000000000042")

    def test_pending_candidate_from_native_driver_crash_is_allowed(self):
        (self.path / "result.txt").unlink()
        self.assertEqual(validator.inspect_capture(self.path)["vk_result"], "pending")

    def test_missing_truncated_oversized_and_omitted_payloads_rejected(self):
        for data in (self.spirv[:-1], b"bad!" * 5, bytes(2 * 1024 * 1024 + 4)):
            with self.subTest(length=len(data)):
                (self.path / "shader.spv").write_bytes(data)
                with self.assertRaises(ValueError):
                    validator.inspect_capture(self.path)
        (self.path / "shader.spv").write_bytes(self.spirv)
        (self.path / "metadata.txt").write_text("status=omitted_payload_unavailable\n")
        with self.assertRaises(ValueError):
            validator.inspect_capture(self.path)

    def test_payload_size_mismatch_is_rejected(self):
        (self.path / "guest.bin").write_bytes(b"different")
        with self.assertRaises(ValueError):
            validator.inspect_capture(self.path)

if __name__ == "__main__":
    unittest.main()
