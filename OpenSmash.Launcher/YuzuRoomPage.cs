using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Windows.Forms;

namespace OpenSmash.Launcher;

// Embedded in LauncherForm's view area. Real input controls retain IME, accessibility
// and keyboard support; cards and buttons use the launcher's existing Theme.
internal sealed class YuzuRoomPage : UserControl
{
    readonly string repo;
    readonly TextBox server = new() { Name = "server", PlaceholderText = "room.example:24872" };
    readonly TextBox nickname = new() { Name = "nickname", MaxLength = 20 };
    readonly TextBox password = new() { Name = "password", UseSystemPasswordChar = true };
    string selectedProfile = "eden";
    string statusText = "Enter a room address, then connect when you are ready.";
    bool statusError;
    string? connectionError;
    readonly TextBox activity = new() { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, TabStop = false, Name = "activity" };
    readonly RoomButton start = new("CONNECT & PLAY", "connect", true);
    readonly RoomButton check = new("CHECK SETUP", "check");
    readonly RoomButton save = new("SAVE", "save");
    readonly RoomButton eden = new("EDEN / COMPATIBLE", "eden");
    readonly RoomButton legacy = new("HISTORICAL YUZU", "legacy");
    readonly RoomButton connectionLog = new("CONNECTION LOG", "connection-log");
    readonly RoomButton preparationLog = new("SETUP LOG", "preparation-log");
    readonly System.Windows.Forms.Timer timer = new() { Interval = 700 };
    readonly List<Control> inputs = new();
    readonly Queue<string> recent = new();
    readonly Func<bool>? saveGameSettings;
    bool busy;
    StreamReader? log;
    float scale = 1;
    public event Action? BusyChanged;
    public Process? GameProcess { get; private set; }
    public bool IsRunning => GameProcess != null && !GameProcess.HasExited;
    public bool IsBusy => busy;
    public string StatusText => statusText;
    protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
    {
        if (keyData == Keys.Escape) { BackRequested?.Invoke(); return true; }
        return base.ProcessCmdKey(ref msg, keyData);
    }
    public event Action? BackRequested;

    public YuzuRoomPage(string repo, Func<bool>? saveGameSettings = null)
    {
        this.repo = repo;
        this.saveGameSettings = saveGameSettings;
        Name = "yuzu-room-page";
        SetStyle(ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
        BackColor = Theme.Bg;
        ForeColor = Theme.Text;
        AutoScaleMode = AutoScaleMode.None;
        Size = new Size(1088, 440);
        var boxes = new[] { server, nickname, password };
        foreach (TextBox c in boxes)
        {
            c.Font = Theme.Body(16); c.BorderStyle = BorderStyle.None; c.BackColor = Theme.Bg3; c.ForeColor = Theme.Text;
            c.AccessibleName = c.Name; c.TabIndex = inputs.Count;
            c.Enter += (_, _) => Invalidate(); c.Leave += (_, _) => Invalidate();
            Controls.Add(c); inputs.Add(c);
            c.KeyDown += async (_, e) => {
                if (e.KeyCode == Keys.Enter) { e.SuppressKeyPress = true; await Launch(false); }
            };
        }
        activity.Visible = false;
        activity.Font = Theme.Body(12); activity.BackColor = Theme.Bg2; activity.ForeColor = Theme.Muted; activity.BorderStyle = BorderStyle.None;
        activity.AccessibleName = "Preparation and connection activity";
        Controls.Add(activity);
        int tab = 3;
        foreach (var b in new[] { eden, legacy, start, check, save, connectionLog, preparationLog })
        {
            b.TabIndex = tab++; Controls.Add(b);
        }
        start.Click += async (_, _) => await Launch(false);
        check.Click += async (_, _) => await Launch(true);
        save.Click += (_, _) => Save();
        eden.Click += (_, _) => { selectedProfile = "eden"; UpdateEnabled(); };
        legacy.Click += (_, _) => { selectedProfile = "yuzu"; UpdateEnabled(); };
        connectionLog.Click += (_, _) => OpenLog("yuzu-client.log");
        preparationLog.Click += (_, _) => OpenLog("yuzu-ui-prepare.log");
        timer.Tick += (_, _) => PollGame();
        try
        {
            var saved = YuzuRoomSettings.Load(repo);
            server.Text = saved.Server; nickname.Text = saved.Nickname;
            selectedProfile = saved.Profile;
            if (selectedProfile != "eden" && selectedProfile != "yuzu")
                SetStatus("The saved format is unsupported. Select a compatibility format below.", true);
        }
        catch (Exception e) { nickname.Text = "OpenSmash"; SetStatus("Saved settings could not be read: " + e.Message, true); }
        UpdateEnabled();
    }

    public void LayoutPage(float s)
    {
        scale = s;
        int X(float value) => (int)Math.Round(value * s);
        void Place(Control c, float x, float y, float w, float h) => c.SetBounds(X(x), X(y), X(w), X(h));
        float leftWidth = Width / s * .57f;
        float rightX = leftWidth + 28, rightWidth = Width / s - rightX;
        foreach (TextBox box in inputs)
        {
            if (Math.Abs(box.Font.Size - 16 * s) > .1) { var old = box.Font; box.Font = Theme.Body(16 * s); old.Dispose(); }
        }
        Place(server, 20, 66, leftWidth - 40, 26);
        Place(nickname, 20, 148, leftWidth * .48f - 30, 26);
        Place(password, leftWidth * .48f + 20, 148, leftWidth * .52f - 40, 26);
        Place(eden, 0, 227, leftWidth * .51f - 5, 42);
        Place(legacy, leftWidth * .51f + 5, 227, leftWidth * .49f - 5, 42);
        Place(start, 0, 333, leftWidth * .48f, 50);
        Place(check, leftWidth * .48f + 12, 333, leftWidth * .31f - 12, 50);
        Place(save, leftWidth * .79f + 12, 333, leftWidth * .21f - 12, 50);
        Place(activity, rightX + 22, 170, rightWidth - 44, 162);
        if (Math.Abs(activity.Font.Size - 12 * s) > .1) { var old = activity.Font; activity.Font = Theme.Body(12 * s); old.Dispose(); }
        Place(connectionLog, rightX + 20, 351, (rightWidth - 48) * .58f, 36);
        Place(preparationLog, rightX + 28 + (rightWidth - 48) * .58f, 351, (rightWidth - 48) * .42f, 36);
        foreach (RoomButton b in Controls.OfType<RoomButton>()) b.UiScale = s;
        eden.Selected = selectedProfile == "eden";
        legacy.Selected = selectedProfile == "yuzu";
        Invalidate();
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        var g = e.Graphics; float s = scale, lw = Width / s * .57f, rx = lw + 28, rw = Width / s - rx;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        void Text(string text, float x, float y, float width, float height, float size, Color color, bool bold = false)
        {
            using var font = Theme.Body(size * s, bold);
            using var brush = new SolidBrush(color);
            g.DrawString(text, font, brush, new RectangleF(x*s,y*s,width*s,height*s));
        }
        void Field(float x, float y, float w, Control input)
        {
            using var fill = new SolidBrush(Theme.Bg3); using var border = new Pen(input.Focused ? Theme.Accent : Theme.Line, Math.Max(1,s));
            g.FillRectangle(fill,x*s,y*s,w*s,48*s); g.DrawRectangle(border,x*s,y*s,w*s,48*s);
        }
        Text("ROOM DETAILS",0,0,lw,24,13,Theme.Muted,true);
        Text("SERVER",0,28,lw,24,11,Theme.Dim,true); Field(0,53,lw,server);
        Text("NICKNAME",0,111,lw*.48f,22,11,Theme.Dim,true);
        Text("PASSWORD  /  NOT SAVED",lw*.48f+8,111,lw*.52f-8,22,11,Theme.Dim,true);
        Field(0,135,lw*.48f-8,nickname); Field(lw*.48f+8,135,lw*.52f-8,password);
        Text("COMPATIBILITY",0,199,lw,22,11,Theme.Dim,true);
        Text("Eden is the default. Use Historical yuzu for older clients.",0,281,lw,37,13,Theme.Muted);
        Text("Your server and nickname are remembered. Password stays private to this launch.",0,398,lw,36,12,Theme.Dim);
        using (var panel = new SolidBrush(Theme.Bg2)) g.FillRectangle(panel,rx*s,0,rw*s,407*s);
        using (var accent = new SolidBrush(statusError ? Theme.Bad : IsRunning ? Theme.Ready : Theme.Accent)) g.FillRectangle(accent,rx*s,0,3*s,52*s);
        Text(statusError ? "NEEDS ATTENTION" : busy ? "PREPARING" : IsRunning ? "GAME SESSION" : "READY TO CONNECT",rx+22,18,rw-44,24,13,statusError ? Theme.Bad : Theme.Text,true);
        Text(statusText,rx+22,55,rw-44,93,15,statusError ? Theme.Bad : Theme.Muted);
        using(var line = new Pen(Theme.Line)) g.DrawLine(line,(rx+22)*s,151*s,(rx+rw-22)*s,151*s);
        if (!activity.Visible)
        {
            Text("01   CONNECT TO THE ROOM",rx+22,178,rw-44,26,12,Theme.Text,true);
            Text("Use the same server as your friends.",rx+22,208,rw-44,34,13,Theme.Muted);
            Text("02   OPEN LOCAL WIRELESS",rx+22,253,rw-44,26,12,Theme.Text,true);
            Text("Create a Room, or find a game in Rooms List.",rx+22,283,rw-44,44,13,Theme.Muted);
        }
        Text("Use Local Wireless in-game. Current enabled mods apply; rollback and run-ahead are off.",rx+4,416,rw-8,35,11,Theme.Dim);
    }

    void OpenLog(string name)
    {
        try
        {
            string path = Path.Combine(repo,"build",name);
            if (!File.Exists(path)) { SetStatus("No log yet. Run Check setup or Connect & play first.", false); return; }
            var psi = new ProcessStartInfo("notepad.exe") { UseShellExecute = false };
            psi.ArgumentList.Add(path); Process.Start(psi);
        }
        catch (Exception e) { SetStatus(e.Message,true); }
    }
    YuzuRoomSettings ReadSettings() => new() { Server = server.Text.Trim(), Nickname = nickname.Text, Profile = selectedProfile };
    void SetStatus(string text, bool error) { statusText = text; statusError = error; Invalidate(); }
    void UpdateEnabled()
    {
        bool edit = !busy && !IsRunning;
        foreach (Control c in inputs) c.Enabled = edit;
        start.Enabled = check.Enabled = save.Enabled = edit;
        eden.Enabled = legacy.Enabled = edit;
        BusyChanged?.Invoke();
        eden.Selected = selectedProfile == "eden"; legacy.Selected = selectedProfile == "yuzu";
        Invalidate();
    }
    void Save()
    {
        try { ReadSettings().Save(repo); SetStatus("Settings saved. Password was not saved.", false); }
        catch (Exception e) { SetStatus(e.Message, true); }
    }
    async Task Launch(bool checkOnly)
    {
        if (busy || IsRunning) return;
        var settings = ReadSettings();
        if (settings.Validate(password.Text) is string error) { SetStatus(error, true); return; }
        busy = true; connectionError = null; UpdateEnabled(); recent.Clear(); activity.Clear(); activity.Visible = true;
        SetStatus(checkOnly ? "Checking setup and preparing plugins…" : "Preparing game and client libraries…", false);
        try
        {
            if (!checkOnly && saveGameSettings != null && !saveGameSettings()) throw new IOException("Game settings could not be saved.");
            Directory.CreateDirectory(Path.Combine(repo, "build"));
            using var output = new StreamWriter(Path.Combine(repo, "build", "yuzu-ui-prepare.log"), false, new System.Text.UTF8Encoding(false)) { AutoFlush = true };
            Process? game = await YuzuRoomLaunch.RunAsync(settings.StartInfo(repo, password.Text, checkOnly), line => {
                output.WriteLine(line);
                if (recent.Count >= 60) recent.Dequeue(); recent.Enqueue(line);
                activity.Text = string.Join(Environment.NewLine, recent); activity.SelectionStart = activity.TextLength; activity.ScrollToCaret();
            });
            if (checkOnly) SetStatus("Setup is ready. No connection was made and room settings were not changed.", false);
            else
            {
                password.Clear();
                GameProcess?.Dispose();
                GameProcess = game;
                log?.Dispose(); log = null;
                if (game == null || game.HasExited)
                {
                    connectionError = YuzuRoomLaunch.EarlyExitMessage(repo);
                    SetStatus(connectionError, true);
                    return;
                }
                SetStatus("Game started. Waiting for the room connection…", false);
                timer.Start(); PollGame();
            }
        }
        catch (Exception e)
        {
            activity.AppendText(Environment.NewLine + e.Message);
            string summary = e.Message.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries).FirstOrDefault() ?? "Unknown error";
            SetStatus("Could not start: " + (summary.Length > 160 ? summary[..160] + "…" : summary), true);
        }
        finally { busy = false; UpdateEnabled(); }
    }
    void PollGame()
    {
        try
        {
            string path = Path.Combine(repo, "build", "yuzu-client.log");
            if (log == null && File.Exists(path)) log = new StreamReader(new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete));
            // Limit each UI tick so a busy runtime cannot freeze the page.
            for (int i = 0; i < 250 && log != null; i++)
            {
                string? line = log.ReadLine(); if (line == null) break;
                if (YuzuRoomLaunch.ConnectionError(line) is string error) connectionError = error;
                if (YuzuRoomLaunch.RoomStatus(line) is string text) SetStatus(text, connectionError != null);
            }
            if (!IsRunning) { timer.Stop(); log?.Dispose(); log = null; SetStatus(connectionError ?? "Game closed. You can change settings and connect again. See the connection log if it exited unexpectedly.", connectionError != null); UpdateEnabled(); }
        }
        catch (IOException) { /* A running game may briefly hold or rotate its log. Try next tick. */ }
    }
    protected override void Dispose(bool disposing)
    {
        if (disposing) { timer.Dispose(); log?.Dispose(); GameProcess?.Dispose(); password.Clear(); }
        base.Dispose(disposing);
    }
}
