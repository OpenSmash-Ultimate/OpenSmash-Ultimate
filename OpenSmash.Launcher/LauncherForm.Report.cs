using System.Diagnostics;

namespace OpenSmash.Launcher;

public partial class LauncherForm
{
    const string BugReportUrl = "https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/issues/new?template=bug_report.yml";

    // The status strip's "Report a problem": Explorer on <install>\build with play.log selected (reports\ holds crash
    // reports), and the bug form with the version filled in.
    bool ActivateReportHit(string id)
    {
        if (id != "report") return false;
        try
        {
            string log = Path.Combine(_buildDir, "play.log");
            if (File.Exists(log)) Process.Start(new ProcessStartInfo("explorer.exe") { Arguments = $"/select,\"{log}\"" });
            else if (Directory.Exists(_buildDir)) Process.Start(new ProcessStartInfo("explorer.exe") { Arguments = $"\"{_buildDir}\"" });
        }
        catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException) { }
        try
        {
            string url = BugReportUrl + (string.IsNullOrEmpty(_version) ? "" : "&version=" + Uri.EscapeDataString(_version));
            Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
        }
        catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException) { }
        return true;
    }
}
