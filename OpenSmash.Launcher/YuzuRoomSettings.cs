using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;

namespace OpenSmash.Launcher;

// The script and GUI share this on-disk schema. Passwords are never part of it.
internal sealed class YuzuRoomSettings
{
    [JsonPropertyName("server")] public string Server { get; set; } = "";
    [JsonPropertyName("nickname")] public string Nickname { get; set; } = "OpenSmash";
    [JsonPropertyName("proxy_profile")] public string Profile { get; set; } = "eden";

    public static string ConfigPath(string repo) => Path.Combine(repo, "config", "yuzu_client.json");
    public static YuzuRoomSettings Load(string repo)
    {
        string path = ConfigPath(repo);
        if (!File.Exists(path)) return new();
        var value = JsonSerializer.Deserialize<YuzuRoomSettings>(File.ReadAllText(path))
            ?? throw new InvalidDataException("The saved room settings are empty.");
        value.Server ??= "";
        value.Nickname ??= "OpenSmash";
        value.Profile = (value.Profile ?? "eden").ToLowerInvariant();
        return value;
    }

    public string? Validate(string password = "")
    {
        if (Server.Length == 0 || Server.Length >= 256 || !Regex.IsMatch(Server, @"\A[a-zA-Z0-9._-]+(?::([0-9]{1,5}))?\z"))
            return "Enter a server hostname or IPv4 address, optionally followed by :port.";
        int colon = Server.LastIndexOf(':');
        if (colon >= 0 && (!int.TryParse(Server[(colon + 1)..], out int port) || port < 1 || port > 65535))
            return "The server port must be between 1 and 65535.";
        if (!Regex.IsMatch(Nickname, @"\A[ a-zA-Z0-9._-]{4,20}\z"))
            return "Use 4–20 letters, digits, spaces, dots, underscores or hyphens for your nickname.";
        if (Profile != "eden" && Profile != "yuzu") return "Select a supported compatibility format.";
        if (password.Contains('\0') || Encoding.UTF8.GetByteCount(password) > 128)
            return "The password must fit in 128 UTF-8 bytes and contain no null characters.";
        return null;
    }

    public void Save(string repo)
    {
        if (Validate() is string error) throw new InvalidDataException(error);
        string path = ConfigPath(repo);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        string temp = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllText(temp, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }), new UTF8Encoding(false));
            File.Move(temp, path, true);
        }
        finally { if (File.Exists(temp)) File.Delete(temp); }
    }

    public ProcessStartInfo StartInfo(string repo, string password, bool checkOnly = false)
    {
        if (Validate(password) is string error) throw new InvalidDataException(error);
        var psi = new ProcessStartInfo("powershell.exe") {
            WorkingDirectory = repo, UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8
        };
        foreach (var arg in new[] { "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
            Path.Combine(repo, "tools", "yuzu_client.ps1"), "-Gui", "-NoPrompt", "-Server", Server,
            "-Nickname", Nickname, "-ProxyProfile", Profile }) psi.ArgumentList.Add(arg);
        if (checkOnly) psi.ArgumentList.Add("-CheckOnly");
        // A stale token/backend from a shell must not affect the GUI's explicit choice.
        foreach (string key in psi.Environment.Keys.Where(k => k.StartsWith("SSBU_", StringComparison.OrdinalIgnoreCase) && !k.Equals("SSBU_CLANG", StringComparison.OrdinalIgnoreCase)).ToArray())
            psi.Environment.Remove(key);
        psi.Environment["SSBU_YUZU_PASSWORD"] = password;
        psi.Environment["PYTHONIOENCODING"] = "utf-8";
        return psi;
    }
}

internal static class YuzuRoomLaunch
{
    public static async Task<int?> RunAsync(ProcessStartInfo start, Action<string> progress)
    {
        using var process = new Process { StartInfo = start };
        try { if (!process.Start()) throw new IOException("The room launcher could not start."); }
        finally { start.Environment.Remove("SSBU_YUZU_PASSWORD"); }
        int? pid = null;
        var errors = new Queue<string>();
        async Task ReadAsync(StreamReader reader, bool stderr)
        {
            while (await reader.ReadLineAsync() is string line)
            {
                if (line.StartsWith("YUZU_RESULT ", StringComparison.Ordinal))
                {
                    using var json = JsonDocument.Parse(line[12..]);
                    pid = json.RootElement.GetProperty("pid").GetInt32();
                }
                else
                {
                    if (stderr) { if (errors.Count == 8) errors.Dequeue(); errors.Enqueue(line); }
                    progress(line);
                }
            }
        }
        await Task.WhenAll(ReadAsync(process.StandardOutput, false), ReadAsync(process.StandardError, true), process.WaitForExitAsync());
        if (process.ExitCode != 0)
            throw new IOException(errors.Count > 0 ? string.Join(Environment.NewLine, errors) : $"Preparation failed (exit {process.ExitCode}). Open the preparation log for details.");
        if (!start.ArgumentList.Contains("-CheckOnly") && (!pid.HasValue || pid <= 0))
            throw new IOException("The launcher finished without reporting a game process. Update tools/yuzu_client.ps1.");
        return pid;
    }

    public static string? RoomStatus(string line)
    {
        if (line.StartsWith("[yuzu] state: "))
        {
            string state = line[14..];
            if (state == "connected") return "Connected to room. In the game, open Local Wireless to host or join a match.";
            if (state == "connecting") return "Connecting to the room server…";
            if (state == "waiting for room admission") return "Connected to server. Waiting for room admission…";
            return "Room connection: " + state;
        }
        if (line.StartsWith("[yuzu] joined ")) return "Connected to room. In the game, open Local Wireless to host or join a match.";
        if (line.StartsWith("[yuzu] wrong room password")) return "Room rejected the password. Close the game, correct it here, and retry.";
        if (line.StartsWith("[yuzu] nickname is invalid or already in use")) return "Nickname is already in use. Close the game and choose another nickname.";
        if (line.StartsWith("[yuzu] room is full")) return "The room is full. Close the game and retry when a place is available.";
        if (line.StartsWith("[yuzu] room protocol version differs")) return "The room uses an unsupported protocol version.";
        if (line.StartsWith("[yuzu] connection lost") || line.StartsWith("[yuzu] room handshake timed out")) return "Connection lost or timed out. The client is retrying.";
        return null;
    }
}
