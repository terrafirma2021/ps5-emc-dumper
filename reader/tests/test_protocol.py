import importlib.util
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "test_emc_log_reader_1360", ROOT / "test_emc_log_reader_1360.py")
probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(probe)


class ImageProtocolTests(unittest.TestCase):
    def setUp(self):
        self.ktext = 0xFFFFFFFFCFE80000
        self.owner = 0xFFFF800000123000
        self.task = (ROOT / "task_gateway.bin").read_bytes()
        self.controller = (ROOT / "controller_gateway.bin").read_bytes()

    def test_two_exact_flat_images_are_embedded_once(self):
        payload = (ROOT / "ps5_emc_log_reader_test_1360.elf").read_bytes()
        self.assertEqual(payload.count(self.task), 1)
        self.assertEqual(payload.count(self.controller), 1)
        self.assertEqual(len(self.task), 0xF60)
        self.assertEqual(len(self.controller), 0xF40)

    def test_corrected_variant_targets_fresh_cbe_cbf_pads(self):
        self.assertEqual(probe.TASK, 0xCBA000)
        self.assertEqual(probe.CONTROLLER, 0xCBF000)
        self.assertEqual(probe.PREVIOUS_CLAIMS,
                         (0xCBB000, 0xCBC000, 0xCBD000, 0xCBE000))
        self.assertEqual(probe.VARIANT_ID,
                          "emc-reader-cba-cbf-sdl-ui-v1")

    def test_readonly_preflight_checks_both_new_pages_entries_and_mappings_twice(self):
        capture = bytearray(probe.SYSENT + 48 * 433)
        capture[probe.TASK:probe.TASK + probe.PAGE] = b"\x90" * probe.PAGE
        capture[probe.CONTROLLER:probe.CONTROLLER + probe.PAGE] = (
            b"\x90" * probe.PAGE)
        capture[probe.RETAINED:probe.RETAINED + probe.PAGE] = b"R" * probe.PAGE
        capture[probe.TASK_ENTRY:probe.TASK_ENTRY + 8] = probe.TASK_ORIGINAL
        capture[probe.CONTROLLER_ENTRY:probe.CONTROLLER_ENTRY + 8] = (
            probe.CONTROLLER_ORIGINAL)
        for number, entry in ((39, probe.CONTROLLER_ENTRY), (432, 0x265C20)):
            struct.pack_into("<Q", capture, probe.SYSENT + 48 * number + 8,
                             self.ktext + entry)
        capture = bytes(capture)
        frames = {probe.TASK: 0x10BE0000, probe.CONTROLLER: 0x10BF0000,
                  probe.TASK_ENTRY: 0x100EA000,
                  probe.CONTROLLER_ENTRY: 0x100E5000}

        class CurrentBoot:
            def __init__(self, corrupt_second_task_read=False):
                self.calls = []
                self.corrupt_second_task_read = corrupt_second_task_read
            def request(self, command, body=b"", response_length=0):
                self.assert_query = (command, body, response_length)
                return struct.pack("<H", 1360)
            def kernel_data_base(self):
                return self_ktext + 0xCC0000
            def read(self, address, length):
                offset = address - self_ktext
                self.calls.append((offset, length))
                if (self.corrupt_second_task_read and offset == probe.TASK and
                        length == probe.PAGE and
                        self.calls.count((offset, length)) == 2):
                    return b"\x90" * (probe.PAGE - 1) + b"\x00"
                return capture[offset:offset + length]

        self_ktext = self.ktext
        checked = []
        def fake_mapping(_debugger, _ktext, address):
            offset = address - self.ktext
            return {"frame": frames[offset], "leaf": 3, "offset": offset}
        def record_mapping(_debugger, row):
            checked.append(row["offset"])

        with patch.object(probe, "CAPTURE_SHA256", hashlib.sha256(capture).hexdigest()), \
             patch.object(probe, "audit_capture", return_value={
                 "getppid_sysent_references": [39]}), \
             patch.object(probe, "verify_dispatch"), \
             patch.object(probe, "mapping", side_effect=fake_mapping), \
             patch.object(probe, "check_mapping", side_effect=record_mapping):
            current = CurrentBoot()
            setup = probe.preflight(current, capture, {})
            for offset in (probe.TASK, probe.CONTROLLER):
                self.assertEqual(current.calls.count((offset, probe.PAGE)), 2)
            for offset in (probe.TASK_ENTRY, probe.CONTROLLER_ENTRY):
                self.assertEqual(current.calls.count((offset, 8)), 2)
            self.assertEqual(checked, [probe.TASK, probe.CONTROLLER,
                                       probe.TASK_ENTRY, probe.CONTROLLER_ENTRY])
            self.assertEqual(setup["fresh_pad_hash"], hashlib.sha256(
                b"\x90" * probe.PAGE).hexdigest())
            with self.assertRaisesRegex(ValueError, "second live read differs"):
                probe.preflight(CurrentBoot(corrupt_second_task_read=True),
                                capture, {})

    def test_aligned_entry_candidates_preserve_padding(self):
        self.assertEqual(probe.candidate(probe.TASK_ENTRY, probe.TASK,
                                         probe.TASK_ORIGINAL),
                          bytes.fromhex("e97bf7a400cccccc"))
        self.assertEqual(probe.candidate(probe.CONTROLLER_ENTRY, probe.CONTROLLER,
                                         probe.CONTROLLER_ORIGINAL),
                          bytes.fromhex("e9eb9ca500cccccc"))
        with self.assertRaises(ValueError):
            probe.candidate(probe.TASK_ENTRY + 1, probe.TASK,
                            probe.TASK_ORIGINAL)

    def test_staged_page_layout_and_non_reusable_claims(self):
        task, ctrl = probe.expected_pages(self.task, self.controller,
                                          self.ktext, self.owner)
        self.assertEqual(len(task), 4096)
        self.assertEqual(len(ctrl), 4096)
        self.assertEqual(struct.unpack_from("<Q", task, 0xF20)[0],
                         self.ktext + probe.TASK_BODY)
        self.assertEqual(struct.unpack_from("<Q", task, 0xF58)[0],
                         self.ktext + 0xA3F4F0)
        self.assertEqual((task[0x301], task[0x302], task[0x308]), (2, 7, 0x20))
        self.assertEqual(struct.unpack_from("<Q", task, probe.CLAIM)[0],
                         probe.RESERVATION ^ self.owner)
        self.assertEqual(struct.unpack_from("<Q", ctrl, probe.CLAIM)[0],
                         (probe.RESERVATION + 1) ^ self.owner)
        self.assertEqual(struct.unpack_from("<Q", ctrl,
                                            probe.CONTROLLER_STATE)[0], self.owner)
        self.assertEqual(struct.unpack_from("<Q", ctrl, probe.REQUEST + 0x38)[0],
                         self.ktext + probe.CONTROLLER + probe.SOURCE)
        self.assertEqual(struct.unpack_from("<Q", ctrl, probe.REQUEST + 0x40)[0],
                         self.ktext + probe.CONTROLLER + probe.REQUEST)
        self.assertEqual(task[0xF60:0xFF0], b"\x90" * (0xFF0 - 0xF60))
        self.assertEqual(ctrl[0xF40:0xFF0], b"\x90" * (0xFF0 - 0xF40))
        with self.assertRaisesRegex(ValueError, "size or zero-state"):
            probe.expected_pages(self.task[:-1], self.controller,
                                 self.ktext, self.owner)

    def test_report_layout_matches_payload(self):
        self.assertEqual(len(probe.REPORT_NAMES), 37)
        self.assertEqual(probe.REPORT.size, 312)
        self.assertEqual(probe.ORDER.size, 48)

    def test_failed_cli_preserves_evidence_and_exits_nonzero(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            capture = directory / "bad-capture.bin"
            evidence = directory / "evidence.json"
            capture.write_bytes(b"bad")
            completed = subprocess.run(
                [sys.executable, str(ROOT / "test_emc_log_reader_1360.py"),
                 "--capture", str(capture), "--out", str(evidence)],
                capture_output=True, text=True, check=False)
            self.assertEqual(completed.returncode, 1)
            record = json.loads(evidence.read_text())
            self.assertEqual(record["status"], "FAILED")
            self.assertEqual(record["probe_variant"], probe.VARIANT_ID)
            self.assertEqual(record["task_gateway_offset"], "0xcba000")
            self.assertEqual(record["controller_gateway_offset"], "0xcbf000")
            self.assertEqual(record["failure_kind"], "contract_or_input_mismatch")
            self.assertEqual(record["preflight_step"], "offline_capture_identity")
            self.assertFalse(record["kernel_writes"])

    def test_readonly_evidence_pins_cbe_cbf_payload_and_rejects_other_image(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            capture = directory / "capture.bin"
            evidence = directory / "cbe-cbf-readonly.json"
            capture.write_bytes(b"placeholder")
            setup = {"ktext": self.ktext, "maps": {},
                     "retained_hash": "R" * 64,
                     "fresh_pad_hash": hashlib.sha256(
                         b"\x90" * probe.PAGE).hexdigest()}
            argv = ["probe", "--capture", str(capture), "--out", str(evidence)]
            with patch.object(probe, "Debugger", return_value=object()), \
                 patch.object(probe, "preflight", return_value=setup), \
                 patch.object(sys, "argv", argv):
                self.assertEqual(probe.main(), 0)
            record = json.loads(evidence.read_text())
            self.assertEqual(record["status"],
                              "LIVE_TASK_GATEWAY_READONLY_PREFLIGHT_PASSED")
            self.assertFalse(record["kernel_writes"])
            self.assertEqual(record["probe_variant"], probe.VARIANT_ID)
            self.assertEqual(record["fresh_pad_sha256"], {
                "cbe_task": setup["fresh_pad_hash"],
                "cbf_controller": setup["fresh_pad_hash"]})
            self.assertEqual(record["payload_sha256"], hashlib.sha256(
                 (ROOT / "ps5_emc_log_reader_test_1360.elf").read_bytes()
            ).hexdigest())

            other = directory / "other.elf"
            other.write_bytes(b"wrong build")
            with patch.object(probe, "Debugger", return_value=object()), \
                 patch.object(probe, "preflight", return_value=setup), \
                 patch.object(sys, "argv", [*argv, "--payload", str(other)]):
                self.assertEqual(probe.main(), 1)
            rejected = json.loads(evidence.read_text())
            self.assertEqual(rejected["status"], "FAILED")
            self.assertFalse(rejected["kernel_writes"])
            self.assertNotIn("launch_requested", rejected)
            self.assertIn("CBE/CBF payload", rejected["error"])

    def test_read_status_eof_is_classified_as_transport_or_service(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            capture = directory / "capture.bin"
            evidence = directory / "evidence.json"
            capture.write_bytes(b"placeholder")
            def interrupted(_debugger, _capture, progress):
                progress["preflight_step"] = "live_syscall_dispatch_read"
                raise EOFError("debugger closed after 0 of 4 bytes")
            argv = ["probe", "--capture", str(capture), "--out", str(evidence)]
            with patch.object(probe, "preflight", interrupted), patch.object(sys,
                    "argv", argv):
                self.assertEqual(probe.main(), 1)
            record = json.loads(evidence.read_text())
            self.assertEqual(record["status"], "FAILED")
            self.assertEqual(record["preflight_step"], "live_syscall_dispatch_read")
            self.assertEqual(record["failure_kind"], "debugger_transport_or_service")
            self.assertFalse(record["kernel_writes"])

    def test_readonly_preflight_retries_identical_read_twice(self):
        class FlakyRead:
            def __init__(self):
                self.calls = []
            def read(self, address, length):
                self.calls.append((address, length))
                if len(self.calls) < 3:
                    raise EOFError("debugger closed after 0 of 4 bytes")
                return b"proof"

        fake = FlakyRead()
        evidence = {"preflight_step": "live_code_read_0x4ea9b0"}
        wrapped = probe.ReadOnlyPreflightDebugger(fake, evidence)
        with patch.object(probe.time, "sleep") as sleep:
            self.assertEqual(wrapped.read(self.ktext + 0x4EA9B0, 5), b"proof")
        self.assertEqual(fake.calls, [(self.ktext + 0x4EA9B0, 5)] * 3)
        self.assertEqual([row["attempt"] for row in evidence["preflight_retries"]],
                         [1, 2])
        self.assertTrue(all(row["step"] == "live_code_read_0x4ea9b0"
                            and row["operation"] == "kernel_read"
                            and row["address"] == hex(self.ktext + 0x4EA9B0)
                            for row in evidence["preflight_retries"]))
        self.assertEqual(sleep.call_count, 2)
        self.assertNotIn("preflight_retry_exhausted", evidence)

    def test_readonly_preflight_exhausts_timeout_and_rejects_other_commands(self):
        class Unavailable:
            def __init__(self):
                self.calls = 0
            def read(self, address, length):
                self.calls += 1
                raise TimeoutError("timed out")
            def request(self, command, body, response_length):
                raise AssertionError("unauthorized command reached debugger")

        fake = Unavailable()
        evidence = {"preflight_step": "live_code_read_0x4ea9b0"}
        wrapped = probe.ReadOnlyPreflightDebugger(fake, evidence)
        with patch.object(probe.time, "sleep"):
            with self.assertRaises(TimeoutError):
                wrapped.read(self.ktext + 0x4EA9B0, 64)
        self.assertEqual(fake.calls, 3)
        self.assertEqual(len(evidence["preflight_retries"]), 2)
        self.assertEqual(evidence["preflight_retry_exhausted"]["attempt"], 3)
        self.assertEqual(evidence["preflight_retry_exhausted"]["step"],
                         "live_code_read_0x4ea9b0")
        with self.assertRaisesRegex(ValueError, "firmware query"):
            wrapped.request(0xDEADBEEF, response_length=2)

        class ContractMismatch:
            def __init__(self):
                self.calls = 0
            def read(self, address, length):
                self.calls += 1
                raise ValueError("invalid live address")

        mismatch = ContractMismatch()
        with self.assertRaisesRegex(ValueError, "invalid live address"):
            probe.ReadOnlyPreflightDebugger(mismatch, {}).read(
                self.ktext + 0x4EA9B0, 64)
        self.assertEqual(mismatch.calls, 1)

    def test_exhausted_readonly_retry_keeps_failed_evidence_and_exit_one(self):
        class ClosedDebugger:
            def __init__(self):
                self.calls = 0
            def read(self, address, length):
                self.calls += 1
                raise EOFError("debugger closed after 0 of 4 bytes")

        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            capture = directory / "capture.bin"
            evidence = directory / "evidence.json"
            capture.write_bytes(b"placeholder")
            fake = ClosedDebugger()
            def read_then_fail(debugger, _capture, progress):
                progress["preflight_step"] = "live_code_read_0x4ea9b0"
                debugger.read(self.ktext + 0x4EA9B0, 64)
            argv = ["probe", "--capture", str(capture), "--out", str(evidence)]
            with patch.object(probe, "Debugger", return_value=fake), patch.object(
                    probe, "preflight", read_then_fail), patch.object(
                    probe.time, "sleep"), patch.object(sys, "argv", argv):
                self.assertEqual(probe.main(), 1)
            record = json.loads(evidence.read_text())
            self.assertEqual(fake.calls, 3)
            self.assertEqual(record["status"], "FAILED")
            self.assertFalse(record["kernel_writes"])
            self.assertEqual(record["preflight_step"], "live_code_read_0x4ea9b0")
            self.assertEqual(record["failure_kind"], "debugger_transport_or_service")
            self.assertEqual(len(record["preflight_retries"]), 2)
            self.assertEqual(record["preflight_retry_exhausted"]["attempt"], 3)

    def test_four_alias_frames_and_independent_entry_restoration(self):
        task, controller, task_entry, controller_entry = (
            0x10B3C000, 0x10B3D000, 0x100EA000, 0x100E5000)
        probe.validate_frames(task, controller, task_entry, controller_entry)
        # Sharing one entry frame would still be safe with independent PTEs
        # and CAS words; the two published code pages must remain separate.
        probe.validate_frames(task, controller, task_entry, task_entry)
        for bad in ((task, task, task_entry, controller_entry),
                    (task, controller, task, controller_entry),
                    (task, controller, task_entry, controller)):
            with self.assertRaises(ValueError):
                probe.validate_frames(*bad)

        class FinalState:
            def __init__(self):
                self.ptes = [0, 0, 0, 0]
                self.controller_phase = 4
                self.task_word = probe.TASK_ORIGINAL
                self.controller_word = probe.CONTROLLER_ORIGINAL
            def read(self, address, length):
                offset = address - self_ktext
                if offset == probe.TASK_ENTRY:
                    return self.task_word
                if offset == probe.CONTROLLER_ENTRY:
                    return self.controller_word
                if offset == probe.TASK + probe.TASK_STATE:
                    state = [0, 0, probe.CLOSED, 0, 0, 1, 1,
                             0, 0, 0, 0]
                    return struct.pack("<11Q", *state)
                if offset == probe.RETAINED:
                    return b"R" * length
                if offset == probe.SYSENT + 39 * 48:
                    return b"S" * length
                raise AssertionError(f"unexpected read {address:#x} {length}")
            def u64(self, address):
                if alias_pte <= address < alias_pte + 32:
                    return self.ptes[(address - alias_pte) // 8]
                if address == self_ktext + probe.CONTROLLER + probe.CONTROLLER_STATE + 0x18:
                    return self.controller_phase
                if address == self_ktext + probe.CONTROLLER + probe.REQUEST + 0x30:
                    return 0
                raise AssertionError(f"unexpected u64 {address:#x}")

        self_ktext = self.ktext
        alias_pte = 0xFFFF8542B0000000
        fake = FinalState()
        import hashlib
        setup = {"ktext": self.ktext,
                 "sysent": {39: b"S" * 48},
                 "retained_hash": hashlib.sha256(b"R" * 4096).hexdigest()}
        self.assertTrue(probe.final_readback(fake, setup, alias_pte)["restored"])
        for slot in range(4):
            fake.ptes[slot] = 1
            self.assertFalse(probe.final_readback(fake, setup, alias_pte)["restored"])
            fake.ptes[slot] = 0
        fake.task_word = probe.candidate(probe.TASK_ENTRY, probe.TASK,
                                         probe.TASK_ORIGINAL)
        self.assertFalse(probe.final_readback(fake, setup, alias_pte)["restored"])
        fake.task_word = probe.TASK_ORIGINAL
        fake.controller_word = probe.candidate(probe.CONTROLLER_ENTRY,
                                               probe.CONTROLLER,
                                               probe.CONTROLLER_ORIGINAL)
        self.assertFalse(probe.final_readback(fake, setup, alias_pte)["restored"])


if __name__ == "__main__":
    unittest.main()
