using System;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace OpenSmash.Launcher
{
    static class Program
    {
        [STAThread]
        static void Main(string[] args)
        {
            // Test entry points (ProjectSetup.RunHeadless): no window, no single-instance check.
            if (ProjectSetup.IsHeadless(args)) { Environment.ExitCode = ProjectSetup.RunHeadless(args); return; }

            // The Play SSBU shortcut: the game straight away once it is built (the launcher waits, hidden, for it).
            if (args.Contains("--play", StringComparer.OrdinalIgnoreCase) && ProjectSetup.FindProject(AppContext.BaseDirectory) is string built
                && WindowSmash.IsBuilt(built))
            {
                using var game = WindowSmash.StartGame(built);
                game?.WaitForExit();
                return;
            }

            ApplicationConfiguration.Initialize();
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            // Per-monitor aware: the windows are drawn at the screen's own resolution. Both forms scale everything
            // they paint from their client height, and Theme.WindowSize sizes them for the monitor's scaling.
            Application.SetHighDpiMode(HighDpiMode.PerMonitorV2);

            // Ensure single instance
            using var mutex = new Mutex(true, "Global\\OpenSmashLauncher", out bool createdNew);
            if (!createdNew) return;

            // Resolve repo root (where the .exe lives, or the repo above its build output folder)
            string repo = ProjectSetup.ExeRoot(AppContext.BaseDirectory);

            // Extract embedded resources (PowerShell scripts, icon) to a temp folder
            string workDir = Path.Combine(Path.GetTempPath(), "OpenSmash_" + Guid.NewGuid().ToString("N")[..8]);
            Directory.CreateDirectory(workDir);
            ExtractResources(workDir);

            // On its own (the release's OpenSmash.exe, or OpenSmash.exe anywhere): the folder set up before,
            // or the first-start screen, which downloads the project and makes the shortcuts. Then the launcher
            // copied into that folder takes over, so everything starts from there as from a copy of the repo.
            if (!ProjectSetup.IsProject(repo))
            {
                string? found = ProjectSetup.FindProject(AppContext.BaseDirectory);
                if (found == null)
                {
                    using var first = new FirstRunForm(workDir, ProjectSetup.DefaultFolder(repo));
                    Application.Run(first);
                    if (first.Folder == "") return;
                    found = first.Folder;
                    string exe = Path.Combine(found, "OpenSmash.exe");
                    if (File.Exists(exe) && !string.Equals(Path.GetFullPath(exe), Environment.ProcessPath, StringComparison.OrdinalIgnoreCase))
                    {
                        mutex.ReleaseMutex();
                        mutex.Dispose();
                        try
                        {
                            Process.Start(new ProcessStartInfo(exe) { WorkingDirectory = found, UseShellExecute = true });
                            return;
                        }
                        catch { }
                    }
                }
                repo = found;
            }

            // No build yet (or --installer): Install WindowSmash comes first.
            if (!WindowSmash.IsBuilt(repo) || args.Contains("--installer", StringComparer.OrdinalIgnoreCase))
            {
                using var installer = new InstallerForm(workDir, repo);
                Application.Run(installer);
                if (!installer.Installed || !WindowSmash.IsBuilt(repo)) return;
            }
            Application.Run(new LauncherForm(workDir, repo, WindowSmash.BuildDir(repo)));
        }

        // ---------------------------------------------------------------------------
        // P/Invoke for DPI awareness
        [DllImport("user32.dll")]
        private static extern IntPtr SetProcessDpiAwarenessContext(IntPtr value);
        private static readonly IntPtr DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = new IntPtr(-4);

        // ---------------------------------------------------------------------------
        // Extract embedded resources to working directory
        static void ExtractResources(string workDir)
        {
            var asm = Assembly.GetExecutingAssembly();
            foreach (string name in asm.GetManifestResourceNames())
            {
                // The marketplace UI is read straight out of the assembly on demand.
                if (name.EndsWith(".html", StringComparison.OrdinalIgnoreCase)) continue;

                // Resources carry their file name as the logical name; fall back to
                // the last two dot-separated segments for the default naming scheme.
                int lastDot = name.LastIndexOf('.');
                int prevDot = lastDot > 0 ? name.LastIndexOf('.', lastDot - 1) : -1;
                string fileName = prevDot >= 0 ? name[(prevDot + 1)..] : name;

                string dest = Path.Combine(workDir, fileName);
                using var stream = asm.GetManifestResourceStream(name);
                if (stream == null) continue;
                using var file = File.Create(dest);
                stream.CopyTo(file);
            }
        }

    }

    // What Install WindowSmash.bat and Play SSBU.bat do, so the window and those files start the same game.
    static class WindowSmash
    {
        static readonly string[] ClearedVars =
        {
            "SSBU_DATA_ROOT", "SSBU_INPUT", "SSBU_NET_HOST", "SSBU_NET_RELAY", "SSBU_NET_BIND",
            "SSBU_NET_ROOM", "SSBU_NET_NAME", "SSBU_NET_DIRECT", "SSBU_NET_PORT"
        };

        public static string BuildDir(string repo) => Path.GetFullPath(Path.Combine(repo, "build"));
        public static bool IsBuilt(string repo) => File.Exists(Path.Combine(BuildDir(repo), "ssbu.exe"));

        public static string ConfigFile(string repo, string name)
        {
            string folder = Path.Combine(repo, "config");
            string inFolder = Path.Combine(folder, name);
            bool made = File.Exists(Path.Combine(folder, "version.ini"));
            if (!made || (name == "online_server.txt" && !File.Exists(inFolder)))
                return Path.Combine(repo, name);
            return inFolder;
        }

        public static string Version(string repo)
        {
            string file = Path.Combine(repo, "version.txt");
            if (!File.Exists(file)) return "";
            foreach (string raw in File.ReadAllLines(file))
            {
                string line = raw.Trim();
                if (line.Length > 0 && line[0] != ';' && line[0] != '#') return line;
            }
            return "";
        }

        // Play SSBU.bat's direct start: build\ssbu.exe dump\modules --status 30 --online, log in build\play.log.
        [DllImport("user32.dll")]
        static extern bool AllowSetForegroundWindow(int dwProcessId);

        public static Process? StartGame(string repo)
        {
            string exe = Path.Combine(BuildDir(repo), "ssbu.exe");
            if (!File.Exists(exe)) return null;
            string log = Path.Combine(BuildDir(repo), "play.log");
            try { File.WriteAllText(log, ""); } catch { }
            var psi = new ProcessStartInfo
            {
                FileName = exe,
                Arguments = "dump\\modules --status 30 --online",
                WorkingDirectory = repo,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };
            foreach (string v in ClearedVars) psi.EnvironmentVariables.Remove(v);
            var logLock = new object();
            var proc = Process.Start(psi);
            if (proc == null) return null;
            try { AllowSetForegroundWindow(proc.Id); } catch { }
            void Write(string? line)
            {
                if (line == null) return;
                lock (logLock)
                {
                    try { File.AppendAllText(log, line + Environment.NewLine); } catch { }
                }
            }
            proc.OutputDataReceived += (_, e) => Write(e.Data);
            proc.ErrorDataReceived += (_, e) => Write(e.Data);
            proc.BeginOutputReadLine();
            proc.BeginErrorReadLine();
            return proc;
        }
    }
}