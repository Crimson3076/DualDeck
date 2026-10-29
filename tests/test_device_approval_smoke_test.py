"""Regression tests for the approval smoke test's bounded log reader."""
import subprocess
import sys
import time
import unittest

from device_approval_smoke_test import wait_for_pending_request


class PendingRequestLogTests(unittest.TestCase):
    def start_process(self, code):
        proc = subprocess.Popen([sys.executable, '-u', '-c', code], stdout=subprocess.PIPE,
                                text=True)
        self.addCleanup(proc.stdout.close)
        self.addCleanup(proc.wait)
        self.addCleanup(proc.kill)
        return proc

    def test_silent_host_times_out(self):
        proc = self.start_process('import time; time.sleep(10)')
        started = time.monotonic()
        with self.assertRaisesRegex(AssertionError, 'deadline'):
            wait_for_pending_request(proc, 'abcdef0123456789', 0.1)
        self.assertLess(time.monotonic() - started, 2)

    def test_partial_line_times_out(self):
        proc = self.start_process("import time; print('partial', end='', flush=True); time.sleep(10)")
        with self.assertRaisesRegex(AssertionError, 'deadline'):
            wait_for_pending_request(proc, 'abcdef0123456789', 0.1)

    def test_reads_handle_after_unrelated_line(self):
        proc = self.start_process("print('starting'); print('pending connection request (device request-2)')")
        self.assertEqual(wait_for_pending_request(proc, 'abcdef0123456789'), 'request-2')

    def test_rejects_credential_prefix_in_log(self):
        proc = self.start_process("print('pending connection request (device abcdef01)')")
        with self.assertRaisesRegex(AssertionError, 'credential prefixes'):
            wait_for_pending_request(proc, 'abcdef0123456789')


if __name__ == '__main__':
    unittest.main()
