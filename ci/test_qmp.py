#!/usr/bin/env python3
"""Regression tests: QMP peer disconnects must fail instead of spinning forever."""
import os
import socket
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import arm64_vm
import kira_vm


class QmpDisconnectTests(unittest.TestCase):
    def closed_pair(self):
        reader, peer = socket.socketpair()
        peer.close()
        return reader

    def test_arm64_qmp_eof_raises(self):
        qmp = arm64_vm.Qmp.__new__(arm64_vm.Qmp)
        qmp.s = self.closed_pair()
        qmp.buf = b""
        try:
            with self.assertRaisesRegex(ConnectionError, "QMP connection closed"):
                qmp.read()
        finally:
            qmp.s.close()

    def test_x86_qmp_eof_raises(self):
        vm = kira_vm.VM.__new__(kira_vm.VM)
        vm.qmp = self.closed_pair()
        try:
            with self.assertRaisesRegex(ConnectionError, "QMP connection closed"):
                vm._qmp_read()
        finally:
            vm.qmp.close()


if __name__ == "__main__":
    unittest.main()
