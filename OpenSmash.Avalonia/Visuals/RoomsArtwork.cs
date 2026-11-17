// The optional yuzu-compatible room client (main's YuzuRoomPage/RoomButton/LauncherForm.Yuzu), Windows only:
// Settings > Advanced > YUZU ROOM opens the "rooms" view. tools/yuzu_client.ps1 prepares and starts the game.
// Text inputs are Avalonia TextBox overlays, like _username/_settingEdit; the rest is painted.
using System.Diagnostics;
using System.Drawing;
using System.Runtime.CompilerServices;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Threading;
using OpenSmash.Core;
using OpenSmash.Launcher;

[assembly: InternalsVisibleTo("OpenSmash.Tests")]

namespace OpenSmash.Visuals;

public sealed partial class ClassicSurface
{
    public static bool RoomsSupported => OperatingSystem.IsWindows();
    TextBox? _roomServer, _roomNick, _roomPassword;
    readonly Rect[] _roomRects = new Rect[3];
    double _roomFont = 16;
    readonly Queue<string> _roomRecent = new();
    string _roomProfile = "eden", _roomStatus = "Enter a room address, then connect when you are ready.";
    string? _roomConnError;
    bool _roomError, _roomBusy;
    Process? _roomGame;
    StreamReader? _roomLog;
    DispatcherTimer? _roomTimer;

    public bool RoomGameRunning => _roomGame is { HasExited: false };
    public bool RoomBusy => _roomBusy;
    public string RoomStatus => _roomStatus;
    internal string RoomProfile => _roomProfile;
    internal Process? RoomGame => _roomGame;
    internal TextBox RoomField(string name) { EnsureRooms(); return name switch { "server" => _roomServer!, "nickname" => _roomNick!, _ => _roomPassword! }; }

    void EnsureRooms()
    {
        if (_roomServer != null) return;
        TextBox Box(string name, string placeholder, int max, bool secret)
        {
            var box = new TextBox { Name = name, PlaceholderText = placeholder, MaxLength = max, IsVisible = false,
                PasswordChar = secret ? '\u2022' : default, FontFamily = _username.FontFamily,
                Background = Avalonia.Media.Brush.Parse(Hex(ArtworkTheme.Bg3)), Foreground = Avalonia.Media.Brush.Parse(Hex(ArtworkTheme.Text)),
                BorderThickness = new Thickness(0), CornerRadius = new CornerRadius(0), Padding = new Thickness(1, 0), MinHeight = 0,
                VerticalContentAlignment = Avalonia.Layout.VerticalAlignment.Center };
            box.KeyDown += (_, e) =>
            {
                if (e.Key == Key.Enter) { _ = RoomLaunch(false); e.Handled = true; }
                else if (e.Key == Key.Escape) { Focus(); Navigate("settings"); e.Handled = true; }
            };
            box.GotFocus += (_, _) => InvalidateVisual(); box.LostFocus += (_, _) => InvalidateVisual();
            LogicalChildren.Add(box); VisualChildren.Add(box);
            return box;
        }
        _roomServer = Box("server", "room.example:24872", 255, false);
        _roomNick = Box("nickname", "", 20, false);
        _roomPassword = Box("password", "", 255, true);
        if (Project == null) return;
        try
        {
            var saved = YuzuRoomSettings.Load(Project.Root);
            _roomServer.Text = saved.Server; _roomNick.Text = saved.Nickname; _roomProfile = saved.Profile;
            if (_roomProfile is not ("eden" or "yuzu")) SetRoomStatus("The saved format is unsupported. Select a compatibility format below.", true);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException)
        { _roomNick.Text = "OpenSmash"; SetRoomStatus("Saved settings could not be read: " + e.Message, true); }
    }

    // Called from UpdateNativeControls and ArrangeOverride.
    void UpdateRoomControls()
    {
        if (_roomServer == null) return;
        bool on = !IsInstaller && _view == "rooms" && !HasPrompt, edit = !_roomBusy && !RoomGameRunning;
        foreach (var box in new[] { _roomServer, _roomNick!, _roomPassword! }) { box.IsVisible = on; box.IsEnabled = edit; }
    }
    void ArrangeRooms()
    {
        if (_roomServer == null) return;
        var boxes = new[] { _roomServer, _roomNick!, _roomPassword! };
        for (int i = 0; i < 3; i++) { boxes[i].FontSize = _roomFont; boxes[i].Measure(_roomRects[i].Size); boxes[i].Arrange(_roomRects[i]); }
    }

    void PaintAdvancedMultiplayer(Graphics g, float x, float width, float top)
    {
        float s = _s;
        ArtworkTheme.Label(g, "Advanced multiplayer", FD(24 * s), A(ArtworkTheme.Muted), x, top, 34 * s, 0.24f * s);
        ArtworkTheme.DrawText(g, "Optional yuzu-compatible rooms", FB(14 * s), A(ArtworkTheme.Dim), x, top + 46 * s, width, 28 * s);
        float y = top + 98 * s, w = Math.Min(width, 300 * s), h = 42 * s;
        bool enabled = !Busy || _roomBusy;
        ArtworkTheme.ButtonAnimated(g, "YUZU ROOM", x, y, w, h, s, false, Hov("nav:rooms", enabled && _hover == "nav:rooms"), enabled, 13 * s, _alpha, press: Press("nav:rooms"));
        if (enabled) AddHit("nav:rooms", x, y, w, h);
    }

    void PaintRooms(Graphics g, int W, int H)
    {
        PaintSectionHead(g, "03 / SETTINGS", "YUZU ROOM");
        EnsureRooms();
        float s = _s, ox = Gutter * s, oy = 212 * s, pw = W - 2 * ox;
        float lw = pw / s * .57f, rx = lw + 28, rw = pw / s - rx;
        float X(float v) => ox + v * s; float Y(float v) => oy + v * s;
        void Text(string text, float x, float y, float w, float h, float size, Color color, bool bold = false, bool wrap = false)
        {
            if (wrap) ArtworkTheme.DrawWrapped(g, text, FB(size * s, bold), A(color), X(x), Y(y), w * s, h * s);
            else ArtworkTheme.DrawText(g, text, FB(size * s, bold), A(color), X(x), Y(y), w * s, h * s);
        }
        bool edit = !_roomBusy && !RoomGameRunning;
        void Field(int i, float x, float y, float w, TextBox box)
        {
            using (var fill = new SolidBrush(A(ArtworkTheme.Bg3))) g.FillRectangle(fill, X(x), Y(y), w * s, 48 * s);
            using (var pen = new Pen(A(box.IsFocused ? ArtworkTheme.Accent : ArtworkTheme.Line), Math.Max(1, s))) g.DrawRectangle(pen, X(x), Y(y), w * s, 48 * s);
            var r = new Rect((X(x) + 14 * s) / _pixelScale, (Y(y) + 10 * s) / _pixelScale, Math.Max(1, (w - 28) * s / _pixelScale), 28 * s / _pixelScale);
            if (r != _roomRects[i] || Math.Abs(box.FontSize - 16 * s / _pixelScale) > .01) { _roomRects[i] = r; _roomFont = 16 * s / _pixelScale; Dispatcher.UIThread.Post(InvalidateArrange); }
        }
        void Button(string id, string label, float x, float y, float w, float h, bool primary, bool enabled, float size = 12)
        {
            ArtworkTheme.ButtonAnimated(g, label, X(x), Y(y), w * s, h * s, s, primary, Hov(id, enabled && _hover == id), enabled, size * s, _alpha, press: Press(id));
            if (enabled) AddHit(id, X(x), Y(y), w * s, h * s);
        }
        Text("ROOM DETAILS", 0, 0, lw, 24, 13, ArtworkTheme.Muted, true);
        Text("SERVER", 0, 28, lw, 24, 11, ArtworkTheme.Dim, true); Field(0, 0, 53, lw, _roomServer!);
        Text("NICKNAME", 0, 111, lw * .48f, 22, 11, ArtworkTheme.Dim, true);
        Text("PASSWORD  /  NOT SAVED", lw * .48f + 8, 111, lw * .52f - 8, 22, 11, ArtworkTheme.Dim, true);
        Field(1, 0, 135, lw * .48f - 8, _roomNick!); Field(2, lw * .48f + 8, 135, lw * .52f - 8, _roomPassword!);
        Text("COMPATIBILITY", 0, 199, lw, 22, 11, ArtworkTheme.Dim, true);
        Button("rooms:eden", "EDEN / COMPATIBLE", 0, 227, lw * .51f - 5, 42, _roomProfile == "eden", edit);
        Button("rooms:legacy", "HISTORICAL YUZU", lw * .51f + 5, 227, lw * .49f - 5, 42, _roomProfile == "yuzu", edit);
        Text("Eden is the default. Use Historical yuzu for older clients.", 0, 281, lw, 37, 13, ArtworkTheme.Muted, wrap: true);
        Button("rooms:connect", "CONNECT & PLAY", 0, 333, lw * .48f, 50, true, edit, 13);
        Button("rooms:check", "CHECK SETUP", lw * .48f + 12, 333, lw * .31f - 12, 50, false, edit);
        Button("rooms:save", "SAVE", lw * .79f + 12, 333, lw * .21f - 12, 50, false, edit);
        Text("Your server and nickname are remembered. Password stays private to this launch.", 0, 398, lw, 36, 12, ArtworkTheme.Dim, wrap: true);

        using (var panel = new SolidBrush(A(ArtworkTheme.Bg2))) g.FillRectangle(panel, X(rx), Y(0), rw * s, 407 * s);
        using (var accent = new SolidBrush(A(_roomError ? ArtworkTheme.Bad : RoomGameRunning ? ArtworkTheme.Ready : ArtworkTheme.Accent))) g.FillRectangle(accent, X(rx), Y(0), 3 * s, 52 * s);
        Text(_roomError ? "NEEDS ATTENTION" : _roomBusy ? "PREPARING" : RoomGameRunning ? "GAME SESSION" : "READY TO CONNECT", rx + 22, 18, rw - 44, 24, 13, _roomError ? ArtworkTheme.Bad : ArtworkTheme.Text, true);
        Text(_roomStatus, rx + 22, 55, rw - 44, 93, 15, _roomError ? ArtworkTheme.Bad : ArtworkTheme.Muted, wrap: true);
        using (var line = new Pen(A(ArtworkTheme.Line), Math.Max(1, s))) g.DrawLine(line, X(rx + 22), Y(151), X(rx + rw - 22), Y(151));
        if (_roomRecent.Count == 0)
        {
            Text("01   CONNECT TO THE ROOM", rx + 22, 178, rw - 44, 26, 12, ArtworkTheme.Text, true);
            Text("Use the same server as your friends.", rx + 22, 208, rw - 44, 34, 13, ArtworkTheme.Muted, wrap: true);
            Text("02   OPEN LOCAL WIRELESS", rx + 22, 253, rw - 44, 26, 12, ArtworkTheme.Text, true);
            Text("Create a Room, or find a game in Rooms List.", rx + 22, 283, rw - 44, 44, 13, ArtworkTheme.Muted, wrap: true);
        }
        else
        {
            var lines = _roomRecent.TakeLast(9).ToArray();
            for (int i = 0; i < lines.Length; i++)
                ArtworkTheme.DrawText(g, lines[i], FM(11 * s), A(ArtworkTheme.Muted), X(rx + 22), Y(166 + i * 18), (rw - 44) * s, 18 * s, localize: false);
        }
        Button("rooms:log:connection", "CONNECTION LOG", rx + 20, 351, (rw - 48) * .58f, 36, false, true);
        Button("rooms:log:setup", "SETUP LOG", rx + 28 + (rw - 48) * .58f, 351, (rw - 48) * .42f, 36, false, true);
        Text("Use Local Wireless in-game. Current enabled mods apply; rollback and run-ahead are off.", rx + 4, 416, rw - 8, 35, 11, ArtworkTheme.Dim, wrap: true);
    }

    bool ActivateRooms(string id)
    {
        if (id == "nav:rooms")
        {
            if (RoomsSupported && Project != null && (!Busy || _roomBusy)) { EnsureRooms(); Navigate("rooms"); }
            return true;
        }
        if (!id.StartsWith("rooms:")) return false;
        bool edit = !_roomBusy && !RoomGameRunning;
        switch (id)
        {
            case "rooms:eden" when edit: _roomProfile = "eden"; break;
            case "rooms:legacy" when edit: _roomProfile = "yuzu"; break;
            case "rooms:save" when edit: SaveRoom(); break;
            case "rooms:connect": _ = RoomLaunch(false); break;
            case "rooms:check": _ = RoomLaunch(true); break;
            case "rooms:log:connection": OpenRoomLog("yuzu-client.log"); break;
            case "rooms:log:setup": OpenRoomLog("yuzu-ui-prepare.log"); break;
        }
        return true;
    }

    YuzuRoomSettings ReadRoomSettings() => new() { Server = (_roomServer?.Text ?? "").Trim(), Nickname = _roomNick?.Text ?? "", Profile = _roomProfile };
    void SetRoomStatus(string text, bool error) { _roomStatus = text; _roomError = error; InvalidateVisual(); }
    void SaveRoom()
    {
        if (Project == null) return;
        try { ReadRoomSettings().Save(Project.Root); SetRoomStatus("Settings saved. Password was not saved.", false); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { SetRoomStatus(e.Message, true); }
    }
    void OpenRoomLog(string name)
    {
        if (Project == null) return;
        string path = Project.PathIn("build", name);
        if (!File.Exists(path)) { SetRoomStatus("No log yet. Run Check setup or Connect & play first.", false); return; }
        try { Commands.OpenPath(path); }
        catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException) { SetRoomStatus(e.Message, true); }
    }
    void SetRoomBusy(bool busy) { _roomBusy = busy; UpdateRoomControls(); InvalidateVisual(); }

    internal async Task RoomLaunch(bool checkOnly)
    {
        if (_roomBusy || RoomGameRunning || Project == null) return;
        EnsureRooms();
        string repo = Project.Root, password = _roomPassword!.Text ?? "";
        var settings = ReadRoomSettings();
        if (settings.Validate(password) is string error) { SetRoomStatus(error, true); return; }
        _roomConnError = null; _roomRecent.Clear(); SetRoomBusy(true);
        SetRoomStatus(checkOnly ? "Checking setup and preparing plugins\u2026" : "Preparing game and client libraries\u2026", false);
        try
        {
            if (!checkOnly)
            {
                if (GameRunning) throw new IOException("Close the current game before starting a room session.");
                if (!ValidateSettingsForPlay()) throw new IOException("Game settings could not be saved.");
            }
            Directory.CreateDirectory(Path.Combine(repo, "build"));
            using var output = new StreamWriter(Path.Combine(repo, "build", "yuzu-ui-prepare.log"), false, new System.Text.UTF8Encoding(false)) { AutoFlush = true };
            var game = await YuzuRoomLaunch.RunAsync(settings.StartInfo(repo, password, checkOnly), line =>
            {
                lock (output) output.WriteLine(line);
                Dispatcher.UIThread.Post(() => { if (_roomRecent.Count >= 60) _roomRecent.Dequeue(); _roomRecent.Enqueue(line); InvalidateVisual(); });
            });
            if (checkOnly) SetRoomStatus("Setup is ready. No connection was made and room settings were not changed.", false);
            else
            {
                _roomPassword.Text = "";
                _roomGame?.Dispose(); _roomGame = game;
                _roomLog?.Dispose(); _roomLog = null;
                if (game == null || game.HasExited) { _roomConnError = YuzuRoomLaunch.EarlyExitMessage(repo); SetRoomStatus(_roomConnError, true); return; }
                SetRoomStatus("Game started. Waiting for the room connection\u2026", false);
                _roomTimer ??= new DispatcherTimer(TimeSpan.FromMilliseconds(700), DispatcherPriority.Background, (_, _) => PollRoomGame());
                _roomTimer.Start(); PollRoomGame();
            }
        }
        catch (Exception e)
        {
            Dispatcher.UIThread.Post(() => { _roomRecent.Enqueue(e.Message); InvalidateVisual(); });
            string summary = e.Message.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries).FirstOrDefault() ?? "Unknown error";
            SetRoomStatus("Could not start: " + (summary.Length > 160 ? summary[..160] + "\u2026" : summary), true);
        }
        finally { SetRoomBusy(false); }
    }
    void PollRoomGame()
    {
        if (Project == null) return;
        try
        {
            string path = Project.PathIn("build", "yuzu-client.log");
            if (_roomLog == null && File.Exists(path)) _roomLog = new StreamReader(new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete));
            // Limit each tick so a busy runtime cannot freeze the page.
            for (int i = 0; i < 250 && _roomLog != null; i++)
            {
                string? line = _roomLog.ReadLine(); if (line == null) break;
                if (YuzuRoomLaunch.ConnectionError(line) is string error) _roomConnError = error;
                if (YuzuRoomLaunch.RoomStatus(line) is string text) SetRoomStatus(text, _roomConnError != null);
            }
            if (!RoomGameRunning)
            {
                _roomTimer?.Stop(); _roomLog?.Dispose(); _roomLog = null;
                SetRoomStatus(_roomConnError ?? "Game closed. You can change settings and connect again. See the connection log if it exited unexpectedly.", _roomConnError != null);
                UpdateRoomControls();
            }
        }
        catch (IOException) { /* A running game may briefly hold or rotate its log. Try next tick. */ }
    }
}
