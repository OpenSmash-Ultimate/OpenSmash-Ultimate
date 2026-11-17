using System.Globalization;
using System.Text.Json;

namespace OpenSmash.Launcher;

// Shared catalogs are the only translation data. UI code and stored settings stay in English.
public static class UiLocale
{
    sealed record Catalog(string Code, string Name, Dictionary<string, string> Messages);
    static readonly Catalog[] catalogs = LoadCatalogs();
    static Catalog current = catalogs[0];
    public static string Preference { get; private set; } = "auto";
    public static string Language => current.Code;
    public static string[] Codes => new[] { "auto" }.Concat(catalogs.Select(c => c.Code)).ToArray();
    public static string[] Names => new[] { "System language" }.Concat(catalogs.Select(c => c.Name)).ToArray();
    public static event Action? Changed;

    static Catalog[] LoadCatalogs()
    {
        var assembly = typeof(UiLocale).Assembly;
        var result = new List<Catalog>();
        foreach (string resource in assembly.GetManifestResourceNames().Where(n => n.StartsWith("Locales.") && n.EndsWith(".json")))
        {
            using var stream = assembly.GetManifestResourceStream(resource)!;
            using var doc = JsonDocument.Parse(stream);
            var data = doc.RootElement;
            var messages = data.GetProperty("messages").Deserialize<Dictionary<string, string>>()!;
            foreach (var entry in messages.ToArray())
                if (!entry.Key.Contains('%') && !entry.Key.Contains('{')) messages.TryAdd(entry.Key.ToUpperInvariant(), entry.Value);
            result.Add(new(resource[8..^5], data.GetProperty("name").GetString()!, messages));
        }
        return result.OrderBy(c => c.Code == "en" ? 0 : 1).ThenBy(c => c.Code, StringComparer.Ordinal).ToArray();
    }

    public static string Resolve(string? code)
    {
        code = (code ?? "").Replace('_', '-').Split('.', '@')[0];
        var exact = catalogs.FirstOrDefault(c => c.Code.Equals(code, StringComparison.OrdinalIgnoreCase));
        return (exact ?? catalogs.FirstOrDefault(c => c.Code.Split('-')[0].Equals(code.Split('-')[0], StringComparison.OrdinalIgnoreCase)) ?? catalogs[0]).Code;
    }

    public static void Initialize(string repo)
    {
        string preference = "auto";
        string file = OpenSmash.Core.Project.ConfigFile(repo, "display.ini");
        if (File.Exists(file))
            foreach (string raw in File.ReadLines(file))
            {
                string line = raw.Trim();
                if (line.StartsWith(';') || line.StartsWith('#')) continue;
                int eq = line.IndexOf('=');
                if (eq >= 0 && line[..eq].Trim().Equals("ui_language", StringComparison.OrdinalIgnoreCase))
                    preference = line[(eq + 1)..].Split(';', '#')[0].Trim();
            }
        Select(preference);
    }

    public static void Select(string? preference)
    {
        Preference = string.IsNullOrWhiteSpace(preference) ? "auto" : preference;
        string? configured = Environment.GetEnvironmentVariable("SSBU_UI_LANGUAGE");
        string code = string.IsNullOrEmpty(configured) ? Preference : configured;
        if (string.IsNullOrEmpty(code) || code.Equals("auto", StringComparison.OrdinalIgnoreCase)) code = CultureInfo.CurrentUICulture.Name;
        var selected = catalogs.Single(c => c.Code == Resolve(code));
        if (ReferenceEquals(selected, current)) return;
        current = selected;
        Changed?.Invoke();
    }

    // Exact source IDs only: paths, player names and arbitrary remote content are never pattern matched.
    public static string Text(string source) => string.IsNullOrEmpty(source) ? source : current.Messages.GetValueOrDefault(source, source);
    public static string Format(string source, params object?[] args) => string.Format(CultureInfo.CurrentCulture, Text(source), args);
    public static string Interpolate(FormattableString source) => Format(source.Format, source.GetArguments());

    // The embedded HTML receives the same catalog; no second translation dictionary in JavaScript.
    internal static string WebScript => "window.__opensmashLocale=" + JsonSerializer.Serialize(new { language = Language, messages = current.Messages }) + ";";
}
