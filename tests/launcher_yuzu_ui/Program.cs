using System.Diagnostics;
using System.Drawing;
using System.Windows.Forms;
using OpenSmash.Launcher;

static class Tests
{
    static int count;
    static void Check(bool ok, string name) { if (!ok) throw new Exception(name); count++; Console.WriteLine("PASS " + name); }
    static Control Find(Control c, string name) => c.Controls.Find(name, true).Single();
    static void Until(Func<bool> condition, string message)
    {
        var until = DateTime.UtcNow.AddSeconds(10);
        while (!condition() && DateTime.UtcNow < until) { Application.DoEvents(); Thread.Sleep(20); }
        Check(condition(), message);
    }
    static void Click(Control page, string name)
    {
        // Manual DoEvents pumping restores the caller context; a real Application.Run
        // keeps it installed. Reinstall it for each synthetic click.
        SynchronizationContext.SetSynchronizationContext(new WindowsFormsSynchronizationContext());
        ((Button)Find(page,name)).PerformClick();
    }
    [STAThread]
    static void Main(string[] args)
    {
        Application.SetHighDpiMode(HighDpiMode.DpiUnaware);
        Application.EnableVisualStyles();
        string repo = Path.Combine(Path.GetTempPath(), "yuzu UI tests " + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(repo);
        try
        {
            var settings = new YuzuRoomSettings { Server = "room.example:24872", Nickname = "TestGuest" };
            Check(settings.Validate() == null, "valid settings");
            Check(settings.Validate(new string('密', 43)) != null, "password UTF-8 byte limit");
            Check(settings.Validate("x\0y") != null, "null password rejected");
            foreach (string server in new[] { "", "bad host", "room:0", "room:65536", "room:abc", "-bad\nargument" })
            {
                settings.Server = server; Check(settings.Validate() != null, "invalid server " + server.Replace('\n',' '));
            }
            settings.Server = "room.example:24872";
            settings.Save(repo);
            var loaded = YuzuRoomSettings.Load(repo);
            Check(loaded.Server == settings.Server && loaded.Profile == "eden", "settings round trip");
            string config = File.ReadAllText(YuzuRoomSettings.ConfigPath(repo));
            Check(!config.Contains("password", StringComparison.OrdinalIgnoreCase), "configuration has no password property");
            File.WriteAllText(YuzuRoomSettings.ConfigPath(repo), "{\"server\":\"old.example\",\"nickname\":\"Legacy\"}");
            Check(YuzuRoomSettings.Load(repo).Profile == "eden", "old config defaults to Eden");
            settings.Save(repo);
            Environment.SetEnvironmentVariable("SSBU_YUZU_TOKEN", "inherited-token");
            Environment.SetEnvironmentVariable("SSBU_NET_HOST", "1");
            var psi = settings.StartInfo(repo, "secret-for-this-launch");
            Check(!string.Join(" ", psi.ArgumentList).Contains("secret-for-this-launch"), "password absent from process arguments");
            Check(psi.Environment["SSBU_YUZU_PASSWORD"] == "secret-for-this-launch", "password in child environment only");
            Check(!psi.Environment.ContainsKey("SSBU_YUZU_TOKEN") && !psi.Environment.ContainsKey("SSBU_NET_HOST"), "inherited backend and token removed");
            Check(psi.ArgumentList.Contains("-Gui") && psi.ArgumentList.Contains("-NoPrompt"), "noninteractive structured launch");
            Check(YuzuRoomLaunch.RoomStatus("[yuzu] tx udp anything") == null, "packet trace is not a connection state");
            Check(YuzuRoomLaunch.RoomStatus("[yuzu] joined room as Guest")!.StartsWith("Connected"), "room admission shown");
            Check(YuzuRoomLaunch.RoomStatus("[yuzu] state: connection lost; retrying")!.Contains("retrying"), "reconnect state replaces connected state");
            Check(YuzuRoomLaunch.RoomStatus("[yuzu] wrong room password")!.Contains("rejected"), "password rejection shown");
            using (var host = new Form())
            using (var form = new YuzuRoomPage(repo))
            {
                host.StartPosition = FormStartPosition.Manual; host.Location = new Point(-20000, -20000);
                host.ClientSize = new Size(1088, 450); host.Controls.Add(form); form.Dock = DockStyle.Fill;
                host.Show(); form.LayoutPage(1); Application.DoEvents();
                SynchronizationContext.SetSynchronizationContext(new WindowsFormsSynchronizationContext());
                Check(((TextBox)Find(form, "password")).UseSystemPasswordChar, "password masked");
                Check(((RoomButton)Find(form, "eden")).Selected, "saved profile loaded into UI");
                ((TextBox)Find(form, "server")).Text = "bad host";
                ((Button)Find(form, "connect")).PerformClick(); Application.DoEvents();
                Check(form.StatusText.Contains("Enter a server"), "invalid connect stays on page");
                ((TextBox)Find(form, "server")).Text = "new.example:5000";
                ((Button)Find(form, "legacy")).PerformClick();
                ((TextBox)Find(form, "password")).Text = "not-saved";
                ((Button)Find(form, "save")).PerformClick();
                Check(YuzuRoomSettings.Load(repo).Profile == "yuzu", "UI saves selected profile");
                Check(!File.ReadAllText(YuzuRoomSettings.ConfigPath(repo)).Contains("not-saved"), "UI never saves password");
                Directory.CreateDirectory(Path.Combine(repo,"tools"));
                string script = Path.Combine(repo,"tools","yuzu_client.ps1");
                File.WriteAllText(script, "param([switch]$Gui,[switch]$NoPrompt,[switch]$CheckOnly,[string]$Server,[string]$Nickname,[string]$ProxyProfile)\nif ($CheckOnly) { Write-Output 'ready'; exit 0 }\n$p = Start-Process powershell.exe -ArgumentList '-NoProfile -Command Start-Sleep -Seconds 30' -WindowStyle Hidden -PassThru\nNew-Item -ItemType Directory -Force build | Out-Null\nSet-Content build/yuzu-client.log '[yuzu] joined fixture as Guest'\nWrite-Output ('YUZU_RESULT {\"pid\":' + $p.Id + '}')\n");
                Click(form,"check");
                Until(() => !form.IsBusy && Find(form,"connect").Enabled, "setup check completes asynchronously");
                Check(form.StatusText.StartsWith("Setup is ready"), "setup check does not claim connection");
                Click(form,"connect");
                try
                {
                    Check(!Find(form,"connect").Enabled, "duplicate connect disabled during preparation");
                    Until(() => !form.IsBusy, "launch finishes asynchronously");
                    Check(form.IsRunning, "child process tracked");
                    Until(() => form.StatusText.StartsWith("Connected"), "actual admission log updates UI");
                    Check(!Find(form,"server").Enabled, "connection inputs locked while game runs");
                    File.AppendAllText(Path.Combine(repo,"build","yuzu-client.log"), "[yuzu] state: connection lost; retrying\r\n");
                    try { Until(() => form.StatusText.Contains("retrying"), "disconnect replaces connected status"); } catch { Console.WriteLine("Actual status: " + form.StatusText + " log=" + File.ReadAllText(Path.Combine(repo,"build","yuzu-client.log"))); throw; }
                }
                finally { if (form.IsRunning) { form.GameProcess!.Kill(true); form.GameProcess.WaitForExit(); } }
                Until(() => Find(form,"connect").Enabled, "game exit enables reconnect");
                File.WriteAllText(script,"Write-Error 'fixture preparation failed'; exit 7");
                Click(form,"connect");
                Until(() => !form.IsBusy && Find(form,"connect").Enabled, "failure releases busy state");
                Check(form.StatusText.StartsWith("Could not start"), "startup error displayed in page");
                Check(Find(form,"connect").Enabled, "failed launch can be retried");
                if (args.Length > 0)
                {
                    Directory.CreateDirectory(args[0]);
                    ((TextBox)Find(form, "server")).Text = "room.example:24872";
                    ((Button)Find(form, "eden")).PerformClick();
                    ((TextBox)Find(form, "password")).Text = "";
                    form.PerformLayout();
                    using var bmp = new Bitmap(form.Width, form.Height); form.DrawToBitmap(bmp, new Rectangle(Point.Empty, form.Size));
                    bmp.Save(Path.Combine(args[0], "yuzu-room-ui.png"));
                    host.ClientSize = new Size(1632, 675); form.LayoutPage(1.5f); form.PerformLayout();
                    using var scaled = new Bitmap(form.Width, form.Height); form.DrawToBitmap(scaled, new Rectangle(Point.Empty, form.Size));
                    scaled.Save(Path.Combine(args[0], "yuzu-room-ui-150.png"));
                }
                host.Close();
            }
            if (args.Length > 0)
            {
                settings.Save(repo);
                using var launcher = new LauncherForm(repo, repo, Path.Combine(repo,"build"), preview: true);
                launcher.ClientSize = new Size(1280,720);
                launcher.StartPosition = FormStartPosition.Manual; launcher.Location = new Point(-20000,-20000);
                launcher.Show(); Application.DoEvents();
                var type = typeof(LauncherForm);
                var flags = System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic;
                type.GetMethod("OpenYuzuRoom",flags)!.Invoke(launcher,null);
                type.GetField("_paintedView",flags)!.SetValue(launcher, "yuzu");
                type.GetField("_viewAt",flags)!.SetValue(launcher, -10d);
                // Paint without showing: no Shown startup work or network.
                type.GetField("_shownAt",flags)!.SetValue(launcher, 0d);
                using var first = new Bitmap(1280,720); launcher.DrawToBitmap(first,new Rectangle(0,0,1280,720));
                Thread.Sleep(1100);
                using var bmp = new Bitmap(1280,720); launcher.DrawToBitmap(bmp,new Rectangle(0,0,1280,720));
                bmp.Save(Path.Combine(args[0],"yuzu-launcher-embedded.png"));
                type.GetMethod("GoView",flags)!.Invoke(launcher,new object[]{"home",true});
                Check(!launcher.Controls.Find("yuzu-room-page",true).Single().Visible,"room controls hidden on home page");
                launcher.Close();
            }
            // Check the process protocol without launching the game or any room connection.
            var fake = new ProcessStartInfo("powershell.exe") { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
            foreach (string a in new[] { "-NoProfile", "-Command", "Write-Output 'YUZU_RESULT {\"pid\":42}'" }) fake.ArgumentList.Add(a);
            fake.Environment["SSBU_YUZU_PASSWORD"] = "secret";
            int? pid = Task.Run(() => YuzuRoomLaunch.RunAsync(fake, _ => {})).GetAwaiter().GetResult();
            Check(pid == 42 && !fake.Environment.ContainsKey("SSBU_YUZU_PASSWORD"), "structured result parsed and secret released");
            Console.WriteLine($"{count} checks passed");
        }
        finally { Directory.Delete(repo, true); }
    }
}
