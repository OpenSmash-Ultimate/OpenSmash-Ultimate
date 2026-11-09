using System.Text.Json;

namespace OpenSmash.Launcher
{
    // Shared process rules, kept separate from the window so they can be tested without WebView2.
    static class ModManagerProcess
    {
        public static string FindPython(string repo, string buildDir)
        {
            string bundled = Path.Combine(repo, "deps", "python", "python.exe");
            if (File.Exists(bundled)) return bundled;

            string configured = Path.Combine(buildDir, "python_path.txt");
            if (File.Exists(configured))
            {
                string candidate = File.ReadAllText(configured).Trim();
                if (File.Exists(candidate)) return candidate;
            }

            // A source build may install its packages in .venv instead of system Python.
            string venv = Path.Combine(repo, ".venv", "Scripts", "python.exe");
            return File.Exists(venv) ? venv : "python";
        }

        public static void ValidateResult(int code, string output)
        {
            string[] lines = output.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
            string result = lines.LastOrDefault(l => l.StartsWith("RESULT ")) ?? "";
            if (result.Length > 0)
            {
                try
                {
                    using var doc = JsonDocument.Parse(result.Substring("RESULT ".Length));
                    var root = doc.RootElement;
                    if (root.ValueKind == JsonValueKind.Object)
                    {
                        if (code == 0 && root.TryGetProperty("ok", out var ok) && ok.ValueKind == JsonValueKind.True)
                            return;
                        if (root.TryGetProperty("error", out var error) && error.ValueKind == JsonValueKind.String
                            && !string.IsNullOrEmpty(error.GetString()))
                            throw new InvalidOperationException(error.GetString());
                    }
                }
                catch (JsonException) { }
            }

            // Import failures happen before Python can emit RESULT; never turn those into success.
            string detail = string.Join(Environment.NewLine, lines.TakeLast(12));
            throw new InvalidOperationException($"The mod manager did not complete (exit {code})."
                + (detail.Length > 0 ? Environment.NewLine + detail : " No result was returned."));
        }
    }
}
