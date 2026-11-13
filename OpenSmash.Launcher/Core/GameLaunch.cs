using System.Diagnostics;

namespace OpenSmash.Core;

public static partial class GameLaunch
{
    // The child writes directly to files, never to pipes owned by the launcher.
    // Disposing its process handles after readiness therefore leaves the game alive.
    public static async Task<int> StartAndWait(Command command, DesktopPlatform platform, string logPath,
        string? nativeLog = null, CancellationToken cancellation = default)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(logPath)!);
        if (nativeLog != null) { Directory.CreateDirectory(Path.GetDirectoryName(nativeLog)!); File.WriteAllText(nativeLog, ""); }
        using var child = OperatingSystem.IsWindows() ? StartWindows(command, logPath) : StartUnix(command, logPath);
        while (!child.HasExited)
        {
            cancellation.ThrowIfCancellationRequested();
            if (child.HasWindow || IsReady(logPath) || nativeLog != null && IsReady(nativeLog)) return child.Id;
            await Task.Delay(100, cancellation);
        }
        throw new InvalidOperationException($"The game stopped before its window opened (exit {child.ExitCode}). See {logPath} for details.");
    }

    static bool IsReady(string path)
    {
        try
        {
            using var input = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            input.Seek(Math.Max(0, input.Length - 65536), SeekOrigin.Begin);
            using var reader = new StreamReader(input);
            return reader.ReadToEnd().Contains("[gfx] window ready", StringComparison.Ordinal);
        }
        catch (IOException) { return false; }
    }

    static Child StartUnix(Command command, string log)
    {
        var original = command.StartInfo();
        string? setsid = OperatingSystem.IsLinux() ? new[]{"/usr/bin/setsid","/bin/setsid"}.FirstOrDefault(File.Exists) : null;
        var info = new ProcessStartInfo(setsid ?? "/bin/bash") { WorkingDirectory = command.WorkingDirectory, UseShellExecute = false, CreateNoWindow = true };
        if (setsid != null) info.ArgumentList.Add("/bin/bash");
        info.ArgumentList.Add("-c");
        info.ArgumentList.Add("exec \"$@\" > \"$OPENSMASH_GAME_LOG\" 2>&1 < /dev/null");
        info.ArgumentList.Add("OpenSmash game");
        info.ArgumentList.Add(command.Executable);
        foreach (string arg in command.Arguments) info.ArgumentList.Add(arg);
        foreach (var entry in original.Environment) info.Environment[entry.Key] = entry.Value;
        foreach (string key in Commands.ClearedGameVariables) info.Environment.Remove(key);
        info.Environment["OPENSMASH_GAME_LOG"] = log;
        return new Child(Process.Start(info) ?? throw new IOException("The game process could not start."));
    }

    sealed partial class Child : IDisposable
    {
        readonly Process? _process;
        public Child(Process process) { _process = process; Id = process.Id; }
        public int Id { get; }
        public bool HasWindow
        {
            get
            {
                if (!OperatingSystem.IsWindows()) return false;
                try { using var process = Process.GetProcessById(Id); process.Refresh(); return IsGameWindow(process.MainWindowHandle); }
                catch (ArgumentException) { return false; }
                catch (InvalidOperationException) { return false; }
            }
        }
        public bool HasExited => _process?.HasExited ?? NativeExited;
        public int ExitCode => _process?.ExitCode ?? NativeExitCode;
        public void Dispose() { _process?.Dispose(); _native?.Dispose(); }
    }
}
