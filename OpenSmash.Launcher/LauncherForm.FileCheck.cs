// SPDX-License-Identifier: GPL-3.0-or-later
// The status strip's "Verify game files" (OpenSmash.Core.FileCheck; SSBU Arena's "Check and repair"): the install is
// checked - nothing is changed - and the result is said. With problems, a repair is offered: only what is broken is made
// again, by the step that makes it (the installer's build step, the plugins' build, Prepare Shaders); the set-up is
// named when the game files themselves are not where they were. OPENSMASH_FILE_CHECK=0: no button.
using OpenSmash.Core;

namespace OpenSmash.Launcher;

public partial class LauncherForm
{
    bool _fileCheckBusy;

    bool ActivateVerifyHit(string id)
    {
        if (id != "verify") return false;
        _ = VerifyGameFilesAsync();
        return true;
    }

    async Task VerifyGameFilesAsync()
    {
        if (_fileCheckBusy || _updateApplying || _modsBusy) return;
        if (_gameProcess != null && !_gameProcess.HasExited) { UiLocale.Message("Close the game first.", "Verify game files"); return; }
        _fileCheckBusy = true;
        try
        {
            SetLoad("VERIFYING THE GAME FILES", -1);
            List<FileProblem> bad;
            bool listed;
            try
            {
                (bad, listed) = await Task.Run(() =>
                {
                    var found = FileCheck.Check(_repo);
                    bool had = FileCheck.HasList(_repo);
                    // (no list yet and nothing to make again: the files as they are now are what the next check compares with)
                    if (!had && !found.Any(p => p.Repair is FileRepair.Build or FileRepair.Setup)) FileCheck.WriteList(_repo);
                    return (found, had);
                });
            }
            finally { ClearLoad(); }
            var repairs = FileCheck.Repairs(bad);
            if (bad.Count == 0)
            {
                UiLocale.Message(FileCheck.Report(bad) + (listed ? "" : "\n\nA list of the installer's files was made: the next check compares against it."), "Verify game files");
                return;
            }
            string report = FileCheck.Report(bad);
            if (repairs.Contains(FileRepair.Setup))
            {
                UiLocale.Message(report + "\n\nThe game files are needed again: run the set-up (Settings > Set up and build) and pick your game folder. Nothing was changed.", "Verify game files");
                return;
            }
            string plan = string.Join(", then ", repairs.Select(r => r == FileRepair.Build ? "the installer's build step (only what is missing is made again)"
                                                               : r == FileRepair.Plugins ? "the plugins built in again" : "Prepare Shaders"));
            if (UiLocale.Message(this, report + "\n\nRepair now? " + plan + ". Your save, settings and mods stay as they are.", "Verify game files",
                                 MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
            string why = "";
            if (repairs.Contains(FileRepair.Build))
            {
                await Task.Run(() => FileCheck.RemoveBroken(_repo, bad));
                try
                {
                    var r = await BuildGameAfterUpdateAsync();
                    if (r.Code != 0) why = "The installer's build step stopped (exit " + r.Code + ").";
                }
                catch (Exception e) when (e is IOException or InvalidOperationException or System.ComponentModel.Win32Exception) { why = e.Message; }
            }
            if (why == "" && repairs.Contains(FileRepair.Plugins)) await BuildPlugins();
            if (why != "") { UiLocale.Message("The repair could not be completed.\n\n" + why, "Verify game files"); return; }
            var left = await Task.Run(() =>
            {
                var again = FileCheck.Check(_repo);
                if (!again.Any(p => p.Repair is FileRepair.Build or FileRepair.Setup)) FileCheck.WriteList(_repo);
                return again;
            });
            if (left.Any(p => p.Repair == FileRepair.Shaders) && !left.Any(p => p.Repair != FileRepair.Shaders))
            {
                _shaderPrompt = true;   // (the launcher's own Prepare Shaders prompt: the priority is the player's to pick)
                PlayPopup();
                Invalidate();
                return;
            }
            UiLocale.Message(left.Count == 0 ? "Repaired. " + FileCheck.Report(left) : "Still to look at:\n" + FileCheck.Report(left), "Verify game files");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { UiLocale.Message("The check stopped: " + e.Message, "Verify game files"); }
        finally { _fileCheckBusy = false; Invalidate(); }
    }
}
