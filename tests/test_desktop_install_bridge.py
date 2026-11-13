"""GUI installer privilege requests use OS dialogs, never captured passwords."""
from pathlib import Path
import os
import sys
import types
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'tools' / 'linux'))
import install_linux as installer


class DesktopAuthorizationTests(unittest.TestCase):
    def invoke(self, gui, pkexec):
        with mock.patch.dict(os.environ, {'SSBU_GUI_INSTALL': gui}), \
                mock.patch.object(installer.os, 'geteuid', return_value=1000, create=True), \
                mock.patch.object(installer.shutil, 'which', return_value='/usr/bin/pkexec' if pkexec else None), \
                mock.patch.object(installer.subprocess, 'run', return_value=types.SimpleNamespace(returncode=0)) as run:
            self.assertTrue(installer.sudo_run(['apt-get', 'update'], False))
            return run.call_args.args[0]

    def test_gui_uses_desktop_authorization(self):
        self.assertEqual(self.invoke('1', True), ['pkexec', 'apt-get', 'update'])

    def test_gui_fails_promptly_without_authorization_agent(self):
        self.assertEqual(self.invoke('1', False), ['sudo', '-n', 'apt-get', 'update'])

    def test_terminal_keeps_its_original_sudo_prompt(self):
        self.assertEqual(self.invoke('0', True), ['sudo', 'apt-get', 'update'])


if __name__ == '__main__':
    unittest.main()
