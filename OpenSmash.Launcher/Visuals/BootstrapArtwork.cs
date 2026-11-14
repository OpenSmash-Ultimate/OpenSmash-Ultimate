using System.Drawing;
using OpenSmash.Core;

namespace OpenSmash.Visuals;

public sealed partial class ClassicSurface
{
    string _bootstrapParent = "", _bootstrapPhase = "download", _bootstrapError = "";
    long _bootstrapDone, _bootstrapTotal = -1;
    bool _bootstrapBusy;
    public bool IsFirstRun => _view == "bootstrap";
    public string BootstrapParent => _bootstrapParent;
    public string BootstrapTarget => ProjectBootstrap.Destination(_bootstrapParent);

    public void ShowFirstRun(string parent)
    {
        Project = null; IsInstaller = true; _view = "bootstrap";
        SetBootstrapParent(parent); UpdateNativeControls(); InvalidateVisual();
    }
    public void SetBootstrapParent(string parent)
    {
        _bootstrapParent = Path.GetFullPath(parent); _bootstrapError = "";
        _status = "Choose where OpenSmash will live, then download and continue.";
        _statusColor = ArtworkTheme.Muted; InvalidateVisual();
    }
    public void StartBootstrap()
    {
        _bootstrapBusy = true; _bootstrapError = ""; _bootstrapPhase = "download";
        _bootstrapDone = 0; _bootstrapTotal = -1; _status = "Getting the project from GitHub...";
        InvalidateVisual();
    }
    public void BootstrapProgress(BootstrapProgress progress)
    {
        _bootstrapPhase = progress.Phase; _bootstrapDone = progress.Done; _bootstrapTotal = progress.Total;
        _status = progress.Phase switch { "extract" => "Unpacking the project...", "verify" => "Checking the download...", "launcher" => "Putting the launcher in your OpenSmash folder...", "complete" => "Ready. Opening setup...", _ => "Downloading from GitHub..." };
        InvalidateVisual();
    }
    public void FinishBootstrap(string error = "")
    {
        _bootstrapBusy = false; _bootstrapError = error;
        _status = error.Length > 0 ? "Download stopped. You can retry or choose another location." : "Ready to continue.";
        InvalidateVisual();
    }

    void PaintBootstrap(Graphics g, int W, int H)
    {
        float s = _s, x = 96 * s, width = W - 2 * x;
        PaintHeader(g, W, "WELCOME TO OPENSMASH", _bootstrapBusy ? "GETTING OPENSMASH" : "CHOOSE YOUR FOLDER",
            "Pick a location. OpenSmash will create its own folder there and take you to setup.");
        SectionLabel(g, "INSTALL LOCATION", x, 142 * s);
        ArtworkTheme.Card(g, x, 166 * s, width, 158 * s, 12 * s, s);
        ArtworkTheme.Label(g, "OPENSMASH", FD(24 * s), ArtworkTheme.Text, x + 24 * s, 187 * s, 28 * s, .24f * s);
        ArtworkTheme.DrawWrapped(g, BootstrapTarget, FM(14 * s), ArtworkTheme.Text, x + 24 * s, 226 * s, width - 210 * s, 56 * s);
        ArtworkTheme.DrawText(g, "Your OpenSmash project and game build will live here.", FB(13 * s), ArtworkTheme.Muted, x + 24 * s, 289 * s, width - 48 * s, 22 * s);
        Pill(g, "bootstrap:browse", "BROWSE", W - x - 136 * s, 230 * s, 112 * s, 32 * s, !_bootstrapBusy);

        if (_bootstrapBusy)
        {
            double fraction = _bootstrapTotal > 0 ? Math.Clamp(_bootstrapDone / (double)_bootstrapTotal, 0, 1) : 0;
            double shown = _bootstrapPhase switch { "extract" => .8 + .18 * fraction, "launcher" or "complete" => .99, "verify" => .8, _ => .8 * fraction };
            string amount = _bootstrapPhase == "download" && _bootstrapTotal <= 0 ? $"{_bootstrapDone / 1048576d:N1} MB" : $"{shown * 100:0}%";
            ArtworkTheme.DrawText(g, amount, FD(58 * s), ArtworkTheme.Text, x - 4 * s, 352 * s, 400 * s, 72 * s);
            float barY = 430 * s, barH = 10 * s;
            using (var track = ArtworkTheme.Para(x, barY, width, barH, barH * .5f))
            using (var brush = new SolidBrush(Color.FromArgb(26, 255, 255, 255))) g.FillPath(brush, track);
            float fill = _bootstrapPhase == "download" && _bootstrapTotal <= 0
                ? (float)(width * .8 * (1 - Math.Exp(-_bootstrapDone / 20e6))) : (float)(width * shown);
            if (fill >= barH)
                using (var bar = ArtworkTheme.Para(x, barY, fill, barH, barH * .5f))
                using (var brush = new SolidBrush(ArtworkTheme.Accent)) g.FillPath(brush, bar);
            string detail = _bootstrapPhase == "extract" ? $"Unpacking {_bootstrapDone:N0} / {_bootstrapTotal:N0} files"
                : _bootstrapPhase == "download" ? _bootstrapTotal > 0 ? $"Downloading {_bootstrapDone / 1048576d:N1} / {_bootstrapTotal / 1048576d:N1} MB" : $"Downloading {_bootstrapDone / 1048576d:N1} MB"
                : _status;
            ArtworkTheme.DrawText(g, detail, FB(14 * s, true), ArtworkTheme.Text, x, 454 * s, width, 28 * s);
            ActionButton(g, "bootstrap:cancel", "CANCEL", x, 574 * s, false);
        }
        else
        {
            if (_bootstrapError.Length > 0)
            {
                ArtworkTheme.Card(g, x, 352 * s, width, 126 * s, 10 * s, s);
                ArtworkTheme.Label(g, "WHAT STOPPED IT", FD(19 * s), ArtworkTheme.Bad, x + 24 * s, 369 * s, 28 * s, .2f * s);
                ArtworkTheme.DrawWrapped(g, _bootstrapError, FB(14 * s), ArtworkTheme.Text, x + 24 * s, 408 * s, width - 48 * s, 56 * s);
            }
            else ArtworkTheme.DrawWrapped(g, "The next screen installs the tools and builds the game from your own game files. You do not need to download the source or build the launcher yourself.",
                FB(16 * s), ArtworkTheme.Muted, x, 364 * s, Math.Min(width, 880 * s), 80 * s);
            float next = ActionButton(g, "bootstrap:download", _bootstrapError.Length > 0 ? "RETRY" : "DOWNLOAD & CONTINUE", x, 574 * s, true);
            ArtworkTheme.Button(g, "USE EXISTING COPY", next, 579 * s, 210 * s, 36 * s, s, false, _hover == "project:choose", true, 13 * s);
            Hit("project:choose", next, 579 * s, 210 * s, 36 * s);
        }
    }
}
