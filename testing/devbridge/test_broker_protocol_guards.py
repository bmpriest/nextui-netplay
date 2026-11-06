"""Regression checks for errors discovered during physical broker runs."""
import importlib.util
from pathlib import Path
import tempfile
import subprocess
from types import SimpleNamespace
import unittest
from unittest import mock

source = Path(__file__).resolve().parents[3] / 'DevBridge.pak/tools/devbridge.py'
spec = importlib.util.spec_from_file_location('devbridge_protocol', source)
protocol = importlib.util.module_from_spec(spec)
spec.loader.exec_module(protocol)


class UntilGuards(unittest.TestCase):
    def test_screen_condition_sends_input_only_on_match(self):
        runner = self.runner()
        runner.verify = mock.Mock()
        runner.protocol = mock.Mock()
        with tempfile.TemporaryDirectory() as root:
            reference = Path(root) / 'reference.png'
            reference.touch()
            runner.execute(f'host:IF_SCREEN {reference} :: false')
        self.assertEqual(runner.protocol.call_count, 1)
        self.assertTrue(runner.protocol.call_args.kwargs['check'])

    def test_screen_condition_mismatch_skips_input_and_records_branch(self):
        runner = self.runner()
        runner.verify = mock.Mock(side_effect=protocol.ProtocolError('screen does not match reference'))
        runner.protocol = mock.Mock()
        events = []
        runner.record = lambda kind, **fields: events.append((kind, fields))
        with tempfile.TemporaryDirectory() as root:
            reference = Path(root) / 'reference.png'
            reference.touch()
            runner.execute(f'host:IF_SCREEN {reference} :: false')
        runner.protocol.assert_not_called()
        self.assertFalse([fields for kind, fields in events if kind == 'branch'][0]['matched'])

    def test_screen_condition_capture_error_is_not_a_mismatch(self):
        runner = self.runner()
        runner.verify = mock.Mock(side_effect=protocol.ProtocolError('capture failed'))
        runner.protocol = mock.Mock()
        with tempfile.TemporaryDirectory() as root:
            reference = Path(root) / 'reference.png'
            reference.touch()
            with self.assertRaisesRegex(protocol.ProtocolError, 'capture failed'):
                runner.execute(f'host:IF_SCREEN {reference} :: false')
        runner.protocol.assert_not_called()

    def adb_transport(self):
        transport = object.__new__(protocol.SSHTransport)
        transport.args = SimpleNamespace(operation_timeout=1)
        return transport, SimpleNamespace(transport='adb', address='fake-serial', alias='host')

    def test_legacy_adb_zero_status_does_not_hide_remote_failure(self):
        transport, device = self.adb_transport()
        completed = subprocess.CompletedProcess([], 0, 'failed check\n\n__DEVBRIDGE_EXIT_0000000000000000__=7\n', '')
        with mock.patch.object(protocol.random, 'getrandbits', return_value=0), mock.patch.object(protocol.subprocess, 'run', return_value=completed):
            result = transport.run(device, 'exit 7', check=False)
        self.assertEqual(result.returncode, 7)
        self.assertEqual(result.stdout, 'failed check\n')

    def test_missing_adb_exit_status_cannot_pass(self):
        transport, device = self.adb_transport()
        completed = subprocess.CompletedProcess([], 0, 'partial output\n', '')
        with mock.patch.object(protocol.subprocess, 'run', return_value=completed):
            with self.assertRaisesRegex(protocol.ProtocolError, 'exit status missing'):
                transport.run(device, 'true')

    def runner(self):
        runner = object.__new__(protocol.Runner)
        device = SimpleNamespace(alias='host')
        runner.devices = {'host': device}
        runner.named_commands = {}
        runner.record = lambda *args, **kwargs: None
        runner.selected = lambda prefix: [device]
        return runner

    def test_missing_reference_never_presses_buttons(self):
        runner = self.runner()
        runner.protocol = lambda *args, **kwargs: self.fail('input sent without a reference')
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaisesRegex(protocol.ProtocolError, 'reference not found'):
                runner.execute(f'host:VERIFY_UNTIL {root}/missing.png 1000 :: false')

    def test_failed_input_command_is_terminal(self):
        runner = self.runner()
        def mismatch(*args, **kwargs):
            raise protocol.ProtocolError('screen mismatch')
        def failed_command(*args, **kwargs):
            self.assertTrue(kwargs['check'])
            raise protocol.ProtocolError('terminal returned 1')
        runner.verify = mismatch
        runner.protocol = failed_command
        with tempfile.TemporaryDirectory() as root:
            reference = Path(root) / 'reference.png'
            reference.touch()
            with self.assertRaisesRegex(protocol.ProtocolError, 'terminal returned 1'):
                runner.execute(f'host:VERIFY_UNTIL {reference} 1000 :: false')


if __name__ == '__main__':
    unittest.main()
