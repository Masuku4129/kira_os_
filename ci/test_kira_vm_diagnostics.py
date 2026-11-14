#!/usr/bin/env python3
"""Unit tests for bounded, phase-aware QEMU guest_errors diagnostics."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kira_vm import count_file_pattern, summarize_qemu_log


class QemuDiagnosticsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = os.path.join(self.tmp.name, "qemu.log")

    def write(self, data):
        with open(self.path, "wb") as f:
            f.write(data)

    def test_pattern_spanning_chunk_boundary_is_counted_once(self):
        self.write(b"abcINVALxyzINVAL")
        self.assertEqual(count_file_pattern(self.path, b"INVAL", chunk_size=6), 2)

    def test_range_boundaries_do_not_leak_counts_between_phases(self):
        self.write(b"Invalid read at addr 0x1\nInvalid read at addr 0x2\n")
        boundary = len(b"Invalid read at addr 0x1\n")
        report = summarize_qemu_log(self.path, boundary)
        self.assertEqual(report["invalid_reads_total"], 2)
        self.assertEqual(report["invalid_reads_before_guest_ready"], 1)
        self.assertEqual(report["invalid_reads_after_guest_ready"], 1)
        self.assertEqual(report["boundary_status"], "available")

    def test_missing_ready_marker_does_not_claim_phase_attribution(self):
        self.write(b"Invalid read at addr 0x1\n")
        report = summarize_qemu_log(self.path, None)
        self.assertEqual(report["invalid_reads_total"], 1)
        self.assertIsNone(report["invalid_reads_before_guest_ready"])
        self.assertIsNone(report["invalid_reads_after_guest_ready"])
        self.assertEqual(report["boundary_status"], "not-recorded")

    def test_boundary_beyond_truncated_log_is_not_trusted(self):
        self.write(b"Invalid read at addr 0x1\n")
        report = summarize_qemu_log(self.path, 1000)
        self.assertEqual(report["invalid_reads_total"], 1)
        self.assertIsNone(report["invalid_reads_before_guest_ready"])
        self.assertIsNone(report["invalid_reads_after_guest_ready"])
        self.assertEqual(report["boundary_status"], "invalid-after-log-truncation")

    def test_missing_log_is_reported_without_exception(self):
        report = summarize_qemu_log(os.path.join(self.tmp.name, "missing.log"), None)
        self.assertFalse(report["exists"])
        self.assertEqual(report["boundary_status"], "log-missing")


if __name__ == "__main__":
    unittest.main()
