"""A copy from before WindowSmash became OpenSmash-Ultimate has a tools/install_windowsmash.ps1 without -UpdateOnly,
so the launcher's Update stopped with "The updater stopped (exit 1)" (#418). The launcher (LauncherForm.cs,
RefreshOldInstallerAsync) then fetches the installer of the commit it updates to and checks it before it puts it in
place: it must declare the -UpdateOnly switch and name the OpenSmash-Ultimate repository. Here: today's installer
passes that check, and the launcher still makes it. Text only: runs anywhere, asks GitHub nothing."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parent.parent
INSTALLER = ROOT / 'tools' / 'install_windowsmash.ps1'
LAUNCHER = ROOT / 'OpenSmash.Launcher' / 'LauncherForm.cs'
TAKES_UPDATE_ONLY = re.compile(r'\[switch\]\s*\$UpdateOnly\b', re.I)   # (LauncherForm.InstallerTakesUpdateOnly)


class OldCopyUpdate(unittest.TestCase):
    def test_todays_installer_passes_the_launchers_check(self):
        text = INSTALLER.read_text(encoding='utf-8-sig')
        self.assertRegex(text, TAKES_UPDATE_ONLY)
        self.assertIn("$RepoName = 'OpenSmash-Ultimate'", text)

    def test_an_installer_without_the_switch_is_replaced_before_it_runs(self):
        text = LAUNCHER.read_text(encoding='utf-8-sig')
        apply = text[text.index('async Task ApplyUpdateAsync()'):]
        apply = apply[:apply.index('RunPowerShellUpdate(script)')]
        self.assertIn('RefreshOldInstallerAsync(script)', apply)
        self.assertIn(r'\[switch\]\s*\$UpdateOnly\b', text)

    def test_a_folder_without_the_launcher_project_still_updates(self):
        # A WindowSmash copy has no OpenSmash.Launcher: the fingerprint Update takes before and after is then "", not
        # an exception that stopped the update before the installer ran; and "" on either side rebuilds nothing.
        text = LAUNCHER.read_text(encoding='utf-8-sig')
        fp = text[text.index('static string LauncherBuildInputsFingerprint('):]
        fp = fp[:fp.index('IncrementalHash')]
        self.assertNotIn('throw', fp)
        self.assertIn('return "";', fp)
        apply = text[text.index('async Task ApplyUpdateAsync()'):]
        apply = apply[:apply.index('ScheduleLauncherBuildAndRestart')]
        self.assertIn('beforeLauncherFingerprint.Length > 0 && afterFingerprint.Length > 0', apply)

    def test_a_new_folder_is_the_latest_release(self):
        # The first start's download is the latest release's ZIP (main only without a release), the ref Update follows.
        setup = (ROOT / 'OpenSmash.Launcher' / 'ProjectSetup.cs').read_text(encoding='utf-8-sig')
        first = (ROOT / 'OpenSmash.Launcher' / 'FirstRunForm.cs').read_text(encoding='utf-8-sig')
        self.assertIn('ProjectSetup.ProjectZipUrlAsync(', first)
        self.assertNotIn('ProjectSetup.MainZipUrl', first)
        self.assertIn('/releases/latest', setup)
        self.assertIn('/zip/refs/tags/', setup)


if __name__ == '__main__':
    unittest.main()
