using System.Drawing;
using System.Windows.Forms;

namespace OpenSmash.Launcher;

// Settings > Controllers > Remap a controller (RemapPage.cs), shown like the yuzu room page: a page of real controls
// in the view's area. SDL is open only while the page is shown, so the game finds the pads free.
public partial class LauncherForm
{
    RemapPage? _remapPage;

    void OpenRemap()
    {
        if (_remapPage == null)
        {
            _remapPage = new RemapPage(_repo, () => new SdlPadSource(_buildDir, _repo)) { Visible = false };
            _remapPage.BackRequested += GoBack;
            Controls.Add(_remapPage);
        }
        GoView("remap");
        _remapPage.Start();
    }

    void LeaveRemap()
    {
        if (_remapPage == null) return;
        _remapPage.Visible = false;
        _remapPage.Stop();
    }

    void PaintRemap(Graphics g, int width, int height)
    {
        PaintSectionHead(g, "03 / SETTINGS", "CONTROLLERS");
        if (_remapPage == null) return;
        var bounds = new Rectangle((int)(Gutter * _s), (int)(212 * _s), width - (int)(2 * Gutter * _s), (int)(450 * _s));
        if (_remapPage.Bounds != bounds) { _remapPage.Bounds = bounds; _remapPage.LayoutPage(_s); }
        bool modal = _shaderPrompt || _pluginPrompt || _newModsPrompt || _updatePrompt || _rebuildPrompt || _sharePrompt || _checkModsPrompt;
        _remapPage.Visible = !modal;
    }
}
