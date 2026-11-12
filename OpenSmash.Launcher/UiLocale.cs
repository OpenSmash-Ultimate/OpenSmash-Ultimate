using System.Globalization;
using System.Text.Json;
using System.Runtime.CompilerServices;

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
        string file = WindowSmash.ConfigFile(repo, "display.ini");
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

    public static DialogResult Message(string text, string caption = "OpenSmash", MessageBoxButtons buttons = MessageBoxButtons.OK,
        MessageBoxIcon icon = MessageBoxIcon.None) => MessageBox.Show(Text(text), Text(caption), buttons, icon);

    public static DialogResult Message(IWin32Window owner, string text, string caption, MessageBoxButtons buttons,
        MessageBoxIcon icon = MessageBoxIcon.None) => MessageBox.Show(owner, Text(text), Text(caption), buttons, icon);

    sealed class ControlText { public string Source = ""; public bool Updating; }
    static readonly ConditionalWeakTable<Control, ControlText> originals = new();

    // Standard WinForms chrome uses the same resources as the owner-drawn screens.
    public static void Attach(Form form)
    {
        void Apply(Control parent)
        {
            if (parent is Label or ButtonBase or GroupBox or Form)
            {
                var value = originals.GetValue(parent, c => new ControlText { Source = c.Text });
                value.Updating = true;
                try { parent.Text = Text(value.Source); }
                finally { value.Updating = false; }
            }
            foreach (Control child in parent.Controls) Apply(child);
        }
        void Track(Control parent)
        {
            if (parent is Label or ButtonBase or GroupBox or Form)
            {
                var value = originals.GetValue(parent, c => new ControlText { Source = c.Text });
                parent.TextChanged += (_, _) => {
                    if (value.Updating) return;
                    value.Source = parent.Text; value.Updating = true;
                    try { parent.Text = Text(value.Source); }
                    finally { value.Updating = false; }
                };
            }
            foreach (Control child in parent.Controls) Track(child);
            parent.ControlAdded += (_, e) => { if (e.Control != null) { Track(e.Control); Apply(e.Control); } };
        }
        void Refresh() { if (!form.IsDisposed) { Apply(form); form.Invalidate(true); } }
        Track(form);
        form.Load += (_, _) => Refresh();
        Changed += Refresh;
        form.Disposed += (_, _) => Changed -= Refresh;
    }

    // The embedded HTML receives the same catalog; no second translation dictionary in JavaScript.
    internal static string WebScript => "window.__opensmashLocale=" + JsonSerializer.Serialize(new { language = Language, messages = current.Messages }) + ";";
}
