using System.Diagnostics;
using System.Drawing;
using System.Windows.Forms;

namespace OpenSmash.Launcher;

public partial class LauncherForm
{
    YuzuRoomPage? _yuzuPage;
    bool YuzuGameRunning() => _yuzuPage?.IsRunning == true;
    void OpenYuzuRoom()
    {
        if (Busy && _yuzuPage?.IsBusy != true) return;
        if (_yuzuPage == null)
        {
            _yuzuPage = new YuzuRoomPage(_repo, () => {
                if (_gameProcess != null && !_gameProcess.HasExited)
                    throw new IOException("Close the current game before starting a room session.");
                return SaveGameSettings();
            });
            _yuzuPage.Visible = false;
            _yuzuPage.BusyChanged += () => Invalidate();
            _yuzuPage.BackRequested += GoBack;
            Controls.Add(_yuzuPage);
        }
        GoView("yuzu");
    }
    void PaintAdvancedMultiplayer(Graphics g, float x, float width, float top)
    {
        float s = _s;
        Theme.Label(g, "Advanced multiplayer", FD(24 * s), A(Theme.Muted), x, top, 34 * s, 0.24f * s);
        Theme.DrawText(g, "Optional yuzu-compatible rooms", FB(14 * s), A(Theme.Dim), x, top + 46 * s, width, 28 * s);
        float y = top + 98 * s, w = Math.Min(width, 300 * s), h = 42 * s;
        bool enabled = !Busy || _yuzuPage?.IsBusy == true;
        Theme.ButtonAnimated(g, "YUZU ROOM", x, y, w, h, s, false, Hov("nav:yuzu", enabled && _hover == "nav:yuzu"), enabled, 13 * s, _alpha, press: Press("nav:yuzu"));
        if (enabled) AddHit("nav:yuzu", x, y, w, h);
    }

    void PaintYuzuRoom(Graphics g, int width, int height)
    {
        PaintSectionHead(g, "03 / SETTINGS", "YUZU ROOM");
        if (_yuzuPage == null) return;
        var bounds = new Rectangle((int)(Gutter * _s), (int)(212 * _s), width - (int)(2 * Gutter * _s), (int)(450 * _s));
        if (_yuzuPage.Bounds != bounds) { _yuzuPage.Bounds = bounds; _yuzuPage.LayoutPage(_s); }
        bool modal = _shaderPrompt || _pluginPrompt || _newModsPrompt || _updatePrompt || _rebuildPrompt || _sharePrompt || _checkModsPrompt;
        _yuzuPage.Visible = !modal;
    }
}
