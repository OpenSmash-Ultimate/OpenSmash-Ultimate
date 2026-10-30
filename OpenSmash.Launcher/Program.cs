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
            ApplicationConfiguration.Initialize();
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.SetHighDpiMode(HighDpiMode.PerMonitorV2);

            // Ensure single instance
            using var mutex = new Mutex(true, "Global\\OpenSmashLauncher", out bool createdNew);
            if (!createdNew) return;

            // DPI awareness for sharp text
            try { SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); } catch { }

            // Resolve repo root (where the .exe lives or where it was launched from)
            string repo = AppContext.BaseDirectory;
            // If running from build output, go up to repo root
            while (repo.EndsWith("OpenSmash.Launcher", StringComparison.OrdinalIgnoreCase) ||
                   repo.EndsWith("bin", StringComparison.OrdinalIgnoreCase) ||
                   repo.EndsWith("Debug", StringComparison.OrdinalIgnoreCase) ||
                   repo.EndsWith("Release", StringComparison.OrdinalIgnoreCase) ||
                   repo.EndsWith("publish", StringComparison.OrdinalIgnoreCase))
            {
                repo = Path.GetFullPath(Path.Combine(repo, ".."));
            }

            // Extract embedded resources (PowerShell scripts, icon) to a temp folder
            string workDir = Path.Combine(Path.GetTempPath(), "OpenSmash_" + Guid.NewGuid().ToString("N")[..8]);
            Directory.CreateDirectory(workDir);
            ExtractResources(workDir);

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
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };
            foreach (string v in ClearedVars) psi.EnvironmentVariables.Remove(v);
            var logLock = new object();
            var proc = Process.Start(psi);
            if (proc == null) return null;
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