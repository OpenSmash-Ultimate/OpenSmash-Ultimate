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
    void PaintYuzuRoom(Graphics g, int width, int height)
    {
        PaintSectionHead(g, "04 / MULTIPLAYER", "YUZU ROOM");
        if (_yuzuPage == null) return;
        var bounds = new Rectangle((int)(Gutter * _s), (int)(212 * _s), width - (int)(2 * Gutter * _s), (int)(450 * _s));
        if (_yuzuPage.Bounds != bounds) { _yuzuPage.Bounds = bounds; _yuzuPage.LayoutPage(_s); }
        bool modal = _shaderPrompt || _pluginPrompt || _newModsPrompt || _updatePrompt || _rebuildPrompt || _sharePrompt || _checkModsPrompt;
        _yuzuPage.Visible = !modal;
    }
}
