using System.Drawing;
using System.Drawing.Drawing2D;
using System.Text;
using System.Windows.Forms;

namespace OpenSmash.Launcher;

// Settings > Controllers > Remap a controller: pick a connected pad, press SET beside a console button, then the pad's
// button for it. Written to config\controllers.ini as [device <GUID>] (PadProfile.cs), which the game takes for that
// model of pad on any port, over [device default] and the pad's type's and name's sections (runtime/hle_pad.c).
internal sealed class RemapPage : UserControl
{
    readonly string repo;
    readonly Func<IPadSource> makeSource;
    IPadSource? source;
    IReadOnlyList<PadDevice> pads = Array.Empty<PadDevice>();
    PadDevice? pad;
    string? capturing;               // the console button waiting for a press
    ISet<string> before = new HashSet<string>();
    DateTime captureUntil;
    string status = "";
    bool statusBad;
    float scale = 1;
    readonly System.Windows.Forms.Timer timer = new() { Interval = 16 };
    readonly List<RoomButton> padButtons = new();
    readonly Dictionary<string, RoomButton> setButtons = new(), clearButtons = new();
    readonly RoomButton dpad = new("D-PAD MOVES: OFF", "dpad_moves"), taunts = new("STICK TAUNTS: OFF", "stick_taunts");
    readonly RoomButton resetAll = new("RESET THIS PAD", "reset"), openFile = new("OPEN CONTROLLERS.INI", "open-file");
    DateTime nextScan;
    public event Action? BackRequested;

    public string StatusText => status;
    public PadDevice? Selected => pad;
    public string? Capturing => capturing;

    public RemapPage(string repo, Func<IPadSource> makeSource)
    {
        this.repo = repo;
        this.makeSource = makeSource;
        Name = "remap-page";
        SetStyle(ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
        BackColor = Theme.Bg;
        ForeColor = Theme.Text;
        AutoScaleMode = AutoScaleMode.None;
        Size = new Size(1088, 440);
        int tab = 10;
        foreach (string b in PadProfile.Buttons)
        {
            var set = new RoomButton("SET", "set-" + b) { TabIndex = tab++ };
            var clear = new RoomButton("DEFAULT", "clear-" + b) { TabIndex = tab++ };
            set.Click += (_, _) => BeginCapture(b);
            clear.Click += (_, _) => Write(b, null);
            setButtons[b] = set; clearButtons[b] = clear;
            Controls.Add(set); Controls.Add(clear);
        }
        foreach (var b in new[] { dpad, taunts, resetAll, openFile }) { b.TabIndex = tab++; Controls.Add(b); }
        dpad.Click += (_, _) => Toggle("dpad_moves");
        taunts.Click += (_, _) => Toggle("stick_taunts");
        resetAll.Click += (_, _) => ResetPad();
        openFile.Click += (_, _) =>
        {
            string path = IniPath;
            if (File.Exists(path)) System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("notepad.exe", "\"" + path + "\"") { UseShellExecute = true });
        };
        timer.Tick += (_, _) => Poll();
    }

    string IniPath => WindowSmash.ConfigFile(repo, "controllers.ini");
    string IniText() { try { return File.Exists(IniPath) ? File.ReadAllText(IniPath) : ""; } catch (IOException) { return ""; } }

    // Shown: SDL opened (the game's own SDL3.dll) and the pads listed; hidden: closed again, so the game gets them.
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
        UpdateButtons();
        timer.Start();
    }

    public void Stop()
    {
        timer.Stop();
        capturing = null;
        source?.Dispose();
        source = null;
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) { Stop(); timer.Dispose(); }
        base.Dispose(disposing);
    }

    protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
    {
        if (keyData == Keys.Escape)
        {
            if (capturing != null) { capturing = null; SetStatus("Cancelled.", false); UpdateButtons(); return true; }
            BackRequested?.Invoke(); return true;
        }
        return base.ProcessCmdKey(ref msg, keyData);
    }

    void Scan()
    {
        if (source == null) { pads = Array.Empty<PadDevice>(); UpdateButtons(); return; }
        var now = source.Pads();
        bool changed = now.Count != pads.Count || now.Where((p, i) => p != pads[i]).Any();
        pads = now;
        if (pad != null && !pads.Contains(pad)) { pad = null; capturing = null; }
        if (pad == null && pads.Count > 0) pad = pads[0];
        if (changed) { RebuildPadButtons(); UpdateButtons(); }
        if (source.Problem == "" && pads.Count == 0) SetStatus("Connect a controller: it shows here as soon as Windows sees it.", false);
        else if (source.Problem == "" && status.StartsWith("Connect a controller")) SetStatus("", false);
    }

    void RebuildPadButtons()
    {
        foreach (var b in padButtons) { Controls.Remove(b); b.Dispose(); }
        padButtons.Clear();
        int i = 0;
        foreach (var p in pads.Take(4))
        {
            var b = new RoomButton(p.Name.ToUpperInvariant(), "pad-" + i++) { TabIndex = i, AccessibleName = p.Name };
            var chosen = p;
            b.Click += (_, _) => { pad = chosen; capturing = null; UpdateButtons(); Invalidate(); };
            padButtons.Add(b); Controls.Add(b);
        }
        LayoutPage(scale);
    }

    // One step of the capture (the timer's tick; the tests call it with their own pad).
    public void Poll()
    {
        if (DateTime.UtcNow >= nextScan && capturing == null) { nextScan = DateTime.UtcNow.AddSeconds(1); Scan(); }
        if (capturing == null || pad == null || source == null) return;
        var now = source.Pressed(pad.Id);
        string? pressed = PadProfile.NewlyPressed(before, now);
        before = new HashSet<string>(before.Intersect(now));   // (released since: a second press of it counts)
        if (pressed != null)
        {
            string b = capturing;
            capturing = null;
            Write(b, pressed);
        }
        else if (DateTime.UtcNow > captureUntil)
        {
            capturing = null;
            SetStatus("Nothing was pressed: press SET to try again.", false);
            UpdateButtons();
        }
    }

    public void BeginCapture(string button)
    {
        if (pad == null || source == null) return;
        capturing = button;
        before = source.Pressed(pad.Id);
        captureUntil = DateTime.UtcNow.AddSeconds(8);
        SetStatus(UiLocale.Format("Press the button for {0} on {1}. Esc cancels.", PadProfile.ButtonLabel(button), pad.Name), false);
        UpdateButtons();
    }

    void Write(string key, string? value)
    {
        if (pad == null) return;
        try
        {
            string text = PadProfile.Set(IniText(), PadProfile.SectionFor(pad.Guid), key, value, pad.Name + " (" + pad.Type + ")");
            Directory.CreateDirectory(Path.GetDirectoryName(IniPath)!);
            File.WriteAllText(IniPath, text, new UTF8Encoding(false));
            if (PadProfile.Buttons.Contains(key))
                SetStatus(value == null ? UiLocale.Format("{0}: back to the default.", PadProfile.ButtonLabel(key))
                                        : UiLocale.Format("{0} is now {1}. The game uses it at its next start.", PadProfile.ButtonLabel(key), PadProfile.SourceLabel(value)), false);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { SetStatus("controllers.ini could not be written: " + e.Message, true); }
        UpdateButtons();
        Invalidate();
    }

    void Toggle(string key)
    {
        if (pad == null) return;
        bool on = PadProfile.Effective(IniText(), pad.Guid, pad.Name, pad.Type, key).Value.Equals("on", StringComparison.OrdinalIgnoreCase);
        Write(key, on ? "off" : "on");
    }

    void ResetPad()
    {
        if (pad == null) return;
        string text = IniText();
        foreach (string k in PadProfile.Buttons.Concat(PadProfile.Switches)) text = PadProfile.Set(text, PadProfile.SectionFor(pad.Guid), k, null);
        try { File.WriteAllText(IniPath, text, new UTF8Encoding(false)); SetStatus("This controller's own buttons are gone: it plays as every other.", false); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { SetStatus("controllers.ini could not be written: " + e.Message, true); }
        UpdateButtons();
        Invalidate();
    }

    void SetStatus(string text, bool bad) { status = text; statusBad = bad; Invalidate(); }

    void UpdateButtons()
    {
        bool ready = pad != null && source != null;
        foreach (var b in PadProfile.Buttons)
        {
            setButtons[b].Enabled = ready;
            setButtons[b].Selected = capturing == b;
            setButtons[b].Text = capturing == b ? "PRESS..." : "SET";
            clearButtons[b].Enabled = ready;
        }
        for (int i = 0; i < padButtons.Count; i++) padButtons[i].Selected = i < pads.Count && pads[i] == pad;
        foreach (var b in new[] { dpad, taunts, resetAll }) b.Enabled = ready;
        if (pad != null)
        {
            string text = IniText();
            dpad.Text = "D-PAD MOVES: " + PadProfile.Effective(text, pad.Guid, pad.Name, pad.Type, "dpad_moves").Value.ToUpperInvariant();
            taunts.Text = "STICK TAUNTS: " + PadProfile.Effective(text, pad.Guid, pad.Name, pad.Type, "stick_taunts").Value.ToUpperInvariant();
        }
        Invalidate();
    }

    public void LayoutPage(float s)
    {
        scale = s;
        int X(float v) => (int)Math.Round(v * s);
        float W = Width / s, colW = (W - 24) / 2;
        for (int i = 0; i < padButtons.Count; i++) padButtons[i].SetBounds(X(i * (W / 4)), X(28), X(W / 4 - 10), X(40));
        for (int i = 0; i < PadProfile.Buttons.Length; i++)
        {
            string b = PadProfile.Buttons[i];
            float x = i < 8 ? 0 : colW + 24, y = 88 + (i % 8) * 34;
            setButtons[b].SetBounds(X(x + colW - 196), X(y), X(84), X(32));
            clearButtons[b].SetBounds(X(x + colW - 106), X(y), X(106), X(32));
        }
        float by = 88 + 8 * 34 + 12;
        dpad.SetBounds(X(0), X(by), X(W * .24f), X(38));
        taunts.SetBounds(X(W * .25f), X(by), X(W * .24f), X(38));
        resetAll.SetBounds(X(W * .52f), X(by), X(W * .22f), X(38));
        openFile.SetBounds(X(W * .76f), X(by), X(W * .24f), X(38));
        foreach (RoomButton b in Controls.OfType<RoomButton>()) b.UiScale = s;
        Invalidate();
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        var g = e.Graphics; float s = scale, W = Width / s, colW = (W - 24) / 2;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        void Text(string text, float x, float y, float width, float height, float size, Color color, bool bold = false)
        {
            using var font = Theme.Body(size * s, bold);
            using var brush = new SolidBrush(color);
            g.DrawString(UiLocale.Text(text), font, brush, new RectangleF(x * s, y * s, width * s, height * s));
        }
        Text(pads.Count == 0 ? "NO CONTROLLER CONNECTED" : "CONTROLLER", 0, 0, W, 24, 13, Theme.Muted, true);
        if (pad != null)
            Text(pad.Type + "   GUID " + pad.Guid, W * .45f, 2, W * .55f, 22, 11, Theme.Dim);
        if (pad != null)
        {
            string ini = IniText();
            for (int i = 0; i < PadProfile.Buttons.Length; i++)
            {
                string b = PadProfile.Buttons[i];
                float x = i < 8 ? 0 : colW + 24, y = 88 + (i % 8) * 34;
                var (value, from) = PadProfile.Effective(ini, pad.Guid, pad.Name, pad.Type, b);
                using (var fill = new SolidBrush(i % 2 == 0 ? Theme.Bg2 : Theme.Bg)) g.FillRectangle(fill, x * s, y * s, (colW - 204) * s, 32 * s);
                Text(PadProfile.ButtonLabel(b), x + 10, y + 6, 130, 22, 13, Theme.Text, true);
                string shown = string.Join(" + ", value.Split(',', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries).Select(PadProfile.SourceLabel));
                Text(shown + (from == "" ? "" : from.Equals(PadProfile.SectionFor(pad.Guid), StringComparison.OrdinalIgnoreCase) ? "" : "  (" + from + ")"),
                     x + 140, y + 7, colW - 350, 22, 12, from == "" ? Theme.Muted : Theme.Accent);
            }
        }
        float sy = 88 + 8 * 34 + 58;
        Text(status != "" ? status : "Changes are saved at once in config\\controllers.ini and used when the game starts.", 0, sy, W, 24, 12, statusBad ? Theme.Bad : Theme.Muted);
    }
}
