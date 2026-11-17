// Settings > Controllers > Remap a controller (main's RemapPage.cs, drawn on the surface): pick a connected pad,
// press SET beside a console button, then the pad's button for it. Written to config/controllers.ini as
// [device <GUID>] (PadProfile.cs), which the game takes for that model of pad on any port, over [device default]
// and the pad's type's and name's sections (runtime/hle_pad.c). SDL is open only while the page is shown, so the
// game finds the pads free.
using System.Drawing;
using System.Text;
using Avalonia.Input;
using Avalonia.Threading;
using OpenSmash.Launcher;

namespace OpenSmash.Visuals;

// The page's state, apart from its drawing (the desktop tests drive it with simulated pads).
internal sealed class RemapState
{
    readonly string iniPath;
    readonly Func<IPadSource> makeSource;
    readonly Func<DateTime> clock;
    IPadSource? source;
    ISet<string> before = new HashSet<string>();
    DateTime captureUntil, nextScan;

    public IReadOnlyList<PadDevice> Pads { get; private set; } = Array.Empty<PadDevice>();
    public PadDevice? Selected { get; private set; }
    public string? Capturing { get; private set; }   // the console button waiting for a press
    public string Status { get; private set; } = "";
    public bool StatusBad { get; private set; }
    public bool Ready => Selected != null && source != null;

    public RemapState(string iniPath, Func<IPadSource> makeSource, Func<DateTime>? clock = null)
    {
        this.iniPath = iniPath;
        this.makeSource = makeSource;
        this.clock = clock ?? (() => DateTime.UtcNow);
    }

    public string IniText() { try { return File.Exists(iniPath) ? File.ReadAllText(iniPath) : ""; } catch (IOException) { return ""; } }

    public void Start()
    {
        if (source == null)
        {
            try { source = makeSource(); }
            catch (Exception e) when (e is DllNotFoundException or BadImageFormatException or EntryPointNotFoundException or InvalidOperationException)
            {
                source = null; SetStatus("Controllers could not be read: " + e.Message, true);
            }
        }
        if (source != null && source.Problem != "") SetStatus(source.Problem, true);
        Scan();
    }

    public void Stop()
    {
        Capturing = null;
        source?.Dispose();
        source = null;
    }

    void Scan()
    {
        nextScan = clock().AddSeconds(1);
        if (source == null) { Pads = Array.Empty<PadDevice>(); Selected = null; return; }
        Pads = source.Pads();
        if (Selected != null && !Pads.Contains(Selected)) { Selected = null; Capturing = null; }
        if (Selected == null && Pads.Count > 0) Selected = Pads[0];
        if (source.Problem == "" && Pads.Count == 0) SetStatus("Connect a controller: it shows here as soon as the system sees it.", false);
        else if (source.Problem == "" && Status.StartsWith("Connect a controller")) SetStatus("", false);
    }

    public void Choose(int index)
    {
        if (index < 0 || index >= Pads.Count) return;
        Selected = Pads[index]; Capturing = null;
    }

    // One step of the capture (the surface's timer; the tests call it with their own pad).
    public void Poll()
    {
        if (clock() >= nextScan && Capturing == null) Scan();
        if (Capturing == null || Selected == null || source == null) return;
        var now = source.Pressed(Selected.Id);
        string? pressed = PadProfile.NewlyPressed(before, now);
        before = new HashSet<string>(before.Intersect(now));   // (released since: a second press of it counts)
        if (pressed != null)
        {
            string b = Capturing;
            Capturing = null;
            Write(b, pressed);
        }
        else if (clock() > captureUntil)
        {
            Capturing = null;
            SetStatus("Nothing was pressed: press SET to try again.", false);
        }
    }

    public void BeginCapture(string button)
    {
        if (Selected == null || source == null || !PadProfile.Buttons.Contains(button)) return;
        Capturing = button;
        before = source.Pressed(Selected.Id);
        captureUntil = clock().AddSeconds(8);
        SetStatus(UiLocale.Format("Press the button for {0} on {1}. Esc cancels.", PadProfile.ButtonLabel(button), Selected.Name), false);
    }

    public bool CancelCapture()
    {
        if (Capturing == null) return false;
        Capturing = null; SetStatus("Cancelled.", false);
        return true;
    }

    public void Write(string key, string? value)
    {
        if (Selected == null) return;
        try
        {
            string text = PadProfile.Set(IniText(), PadProfile.SectionFor(Selected.Guid), key, value, Selected.Name + " (" + Selected.Type + ")");
            Save(text);
            if (PadProfile.Buttons.Contains(key))
                SetStatus(value == null ? UiLocale.Format("{0}: back to the default.", PadProfile.ButtonLabel(key))
                                        : UiLocale.Format("{0} is now {1}. The game uses it at its next start.", PadProfile.ButtonLabel(key), PadProfile.SourceLabel(value)), false);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { SetStatus("controllers.ini could not be written: " + e.Message, true); }
    }

    public string Effective(string key) => Selected == null ? "" : PadProfile.Effective(IniText(), Selected.Guid, Selected.Name, Selected.Type, key).Value;

    public void Toggle(string key)
    {
        if (Selected == null) return;
        Write(key, Effective(key).Equals("on", StringComparison.OrdinalIgnoreCase) ? "off" : "on");
    }

    public void ResetPad()
    {
        if (Selected == null) return;
        string text = IniText();
        foreach (string k in PadProfile.Buttons.Concat(PadProfile.Switches)) text = PadProfile.Set(text, PadProfile.SectionFor(Selected.Guid), k, null);
        try { Save(text); SetStatus("This controller's own buttons are gone: it plays as every other.", false); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { SetStatus("controllers.ini could not be written: " + e.Message, true); }
    }

    void Save(string text)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(iniPath)!);
        File.WriteAllText(iniPath, text, new UTF8Encoding(false));
    }

    void SetStatus(string text, bool bad) { Status = text; StatusBad = bad; }
}

public sealed partial class ClassicSurface
{
    RemapState? _remap;
    DispatcherTimer? _remapTimer;
    internal Func<IPadSource>? PadSourceFactory { get; set; }   // (tests: simulated pads; null: the game's SDL)
    internal RemapState? Remap => _remap;

    void EnterRemap()
    {
        if (Project == null) return;
        string root = Project.Root;
        _remap = new RemapState(Project.ConfigFile("controllers.ini"), PadSourceFactory ?? (() => PadSources.ForProject(root)));
        _remap.Start();
        _remapTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(16) };
        _remapTimer.Tick += (_, _) => { _remap?.Poll(); InvalidateVisual(); };
        _remapTimer.Start();
    }

    void LeaveRemap()
    {
        _remapTimer?.Stop(); _remapTimer = null;
        _remap?.Stop(); _remap = null;
    }

    bool ActivateRemap(string id)
    {
        var r = _remap;
        if (id == "nav:back") { Navigate("settings"); return true; }
        if (r == null) return false;
        if (id.StartsWith("remap:pad:") && int.TryParse(id[10..], out int pad)) r.Choose(pad);
        else if (id.StartsWith("remap:set:")) r.BeginCapture(id[10..]);
        else if (id.StartsWith("remap:clear:")) r.Write(id[12..], null);
        else if (id is "remap:dpad_moves" or "remap:stick_taunts") r.Toggle(id[6..]);
        else if (id == "remap:reset") r.ResetPad();
        else return false;
        return true;
    }

    bool HandleRemapKey(Key key)
    {
        if (key != Key.Escape) return false;
        if (_remap?.CancelCapture() != true) Navigate("settings");
        return true;
    }

    void PaintRemap(Graphics g, int W, int H)
    {
        float s = _s, x = Gutter * s, cw = W - 2 * x, colW = (cw - 24 * s) / 2;
        PaintSectionHead(g, "03 / SETTINGS", "CONTROLLERS");
        var r = _remap;
        var st = PushFade(g, Up((float)(Now - _viewAt), 0.2f), 16 * s);
        float top = ContentTop * s - 4 * s;
        void Btn(string id, string label, float bx, float by, float bw, float bh, bool primary, bool enabled, bool selected = false)
        {
            ArtworkTheme.ButtonAnimated(g, label, bx, by, bw, bh, s, primary || selected, Hov(id, enabled && _hover == id), enabled, 12 * s, _alpha, press: Press(id));
            if (enabled) AddHit(id, bx, by, bw, bh);
        }
        var pads = r?.Pads ?? Array.Empty<PadDevice>();
        var sel = r?.Selected;
        if (pads.Count == 0)
            ArtworkTheme.Label(g, "NO CONTROLLER CONNECTED", FM(12 * s), A(ArtworkTheme.Muted), x, top, 38 * s, 12 * s * 0.16f);
        float pw = (cw - 3 * 10 * s) / 4;
        for (int i = 0; i < Math.Min(4, pads.Count); i++)
            Btn("remap:pad:" + i, pads[i].Name.ToUpperInvariant(), x + i * (pw + 10 * s), top, pw, 38 * s, false, true, pads[i] == sel);
        if (sel != null)
            ArtworkTheme.DrawText(g, sel.Type + "   GUID " + sel.Guid, FM(11 * s), A(ArtworkTheme.Dim), x, top + 42 * s, cw, 18 * s, localize: false);

        float rowsTop = top + 66 * s, rowH = 32 * s, step = 34 * s;
        if (sel != null && r != null)
        {
            string ini = r.IniText();
            for (int i = 0; i < PadProfile.Buttons.Length; i++)
            {
                string b = PadProfile.Buttons[i];
                float rx = x + (i < 8 ? 0 : colW + 24 * s), ry = rowsTop + (i % 8) * step;
                var (value, from) = PadProfile.Effective(ini, sel.Guid, sel.Name, sel.Type, b);
                using (var fill = new SolidBrush(A(White(i % 2 == 0 ? 10 : 4)))) g.FillRectangle(fill, rx, ry, colW - 204 * s, rowH);
                ArtworkTheme.DrawText(g, PadProfile.ButtonLabel(b), FB(13 * s, true), A(ArtworkTheme.Text), rx + 10 * s, ry, 130 * s, rowH);
                string shown = string.Join(" + ", value.Split(',', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries).Select(PadProfile.SourceLabel));
                bool own = from.Equals(PadProfile.SectionFor(sel.Guid), StringComparison.OrdinalIgnoreCase);
                ArtworkTheme.DrawText(g, shown + (from == "" || own ? "" : "  (" + from + ")"), FB(12 * s), A(from == "" ? ArtworkTheme.Muted : ArtworkTheme.Accent),
                    rx + 140 * s, ry, colW - 350 * s, rowH, localize: false);
                bool capturing = r.Capturing == b;
                Btn("remap:set:" + b, capturing ? "PRESS..." : "SET", rx + colW - 196 * s, ry, 84 * s, rowH, false, r.Ready, capturing);
                Btn("remap:clear:" + b, "DEFAULT", rx + colW - 106 * s, ry, 106 * s, rowH, false, r.Ready);
            }
        }
        float by = rowsTop + 8 * step + 12 * s, bh = 38 * s;
        bool ready = r?.Ready == true;
        string Switch(string key) => sel == null || r == null ? "OFF" : r.Effective(key).ToUpperInvariant();
        Btn("remap:dpad_moves", "D-PAD MOVES: " + Switch("dpad_moves"), x, by, cw * .24f, bh, false, ready);
        Btn("remap:stick_taunts", "STICK TAUNTS: " + Switch("stick_taunts"), x + cw * .25f, by, cw * .24f, bh, false, ready);
        Btn("remap:reset", "RESET THIS PAD", x + cw * .52f, by, cw * .22f, bh, false, ready);
        Btn("settings:pads", "OPEN CONTROLLERS.INI", x + cw * .76f, by, cw * .24f, bh, false, Project != null);
        string status = r?.Status is { Length: > 0 } text ? text : "Changes are saved at once in config/controllers.ini and used when the game starts.";
        ArtworkTheme.DrawWrapped(g, status, FB(13 * s), A(r?.StatusBad == true ? ArtworkTheme.Bad : ArtworkTheme.Muted), x, by + bh + 14 * s, cw, 40 * s);
        PopFade(g, st);
    }
}
