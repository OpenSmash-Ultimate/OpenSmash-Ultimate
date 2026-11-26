// The COMPETITIVE page (top menu, between MODS and SETTINGS), as main's LauncherForm.Competitive.cs: SSBU Arena's ranked
// 1v1, the competitive ladder of the same account (OpenSmash itself has mastery points, no ranking). Its top 10 from the
// accounts service's public ranked_board (OpenSmash.Core.RankedBoard; All / EU / NA East / SA: MainWindow.Discord.cs asks it),
// each row with the player's fighters (Max_Hamm's stock icons as ssbuarena.com serves them: OpenSmash.Core.Fighters),
// name#tag, tier (its colour) and rating; the main button starts SSBU Arena when it is installed (Windows: its launcher's
// install_path.txt), else opens its site; "Full leaderboard" opens ssbuarena.com's. An empty board or no answer is said
// quietly. Also here: the fighter icons the home's top mastery and the account card draw.
using System.Drawing;
using System.Globalization;
using OpenSmash.Core;
using SkiaSharp;
namespace OpenSmash.Visuals;

public sealed partial class ClassicSurface
{
    readonly Dictionary<string, Bitmap?> _fighterIcons = new();

    /// <summary>the COMPETITIVE page is offered (OPENSMASH_MASTERY_UI=0 or OPENSMASH_ARENA_CARD=0: not)</summary>
    internal static bool CompetitiveOn => Mastery.UiOn && Mastery.ArenaCardOn;

    Bitmap? FighterIcon(string? fighter)
    {
        string? folder = Fighters.IconFolder(fighter);
        if (folder == null || fighter == null) return null;
        if (_fighterIcons.TryGetValue(folder, out var img) && img != null) return img;
        if (Community.IconBytes?.Invoke(fighter) is not { } bytes) return null;
        img = SKBitmap.Decode(bytes) is { } decoded ? new Bitmap(decoded) : null;
        _fighterIcons[folder] = img;
        return img;
    }

    /// <summary>a fighter's stock icon in a size x size square; without one, its initial on a disc</summary>
    void DrawFighter(Graphics g, string? fighter, float x, float y, float size)
    {
        if (string.IsNullOrEmpty(fighter)) return;
        if (FighterIcon(fighter) is { } img) { g.DrawImage(img, new RectangleF(x, y, size, size), _alpha); return; }
        using (var b = new SolidBrush(A(ArtworkTheme.Bg3))) g.FillEllipse(b, x, y, size, size);
        string name = Fighters.Name(fighter);
        ArtworkTheme.Label(g, name.Length > 0 ? name[..1].ToUpperInvariant() : "?", FM(size * 0.42f, true), A(ArtworkTheme.Muted), x, y, size, 0, StringAlignment.Center, size);
    }

    static Color TierColor(RankedRow r)
    {
        string c = r.TierColor ?? "#8C9AAD";
        try { return Color.FromArgb(255, Convert.ToInt32(c.Substring(1, 2), 16), Convert.ToInt32(c.Substring(3, 2), 16), Convert.ToInt32(c.Substring(5, 2), 16)); }
        catch (FormatException) { return Color.FromArgb(255, 0x8C, 0x9A, 0xAD); }
    }

    void PaintCompetitive(Graphics g, int W, int H)
    {
        var c = Community;
        float s = _s, x = Gutter * s, cw = W - 2 * x;
        PaintSectionHead(g, "03 / COMPETITIVE", "COMPETITIVE");

        // the SSBU Arena card: what it is, the main button and the full leaderboard
        float cy = ContentTop * s, ch = 108 * s, cut = 22 * s, pad = 24 * s;
        {
            var st = PushReveal(g, 0);
            using (var path = ArtworkTheme.CutCorner(x, cy, cw, ch, cut))
            {
                using (var b = new SolidBrush(A(ArtworkTheme.Bg2))) g.FillPath(b, path);
                using (var pen = new Pen(A(ArtworkTheme.Line), Math.Max(1, s))) g.DrawPath(pen, path);
            }
            using (var pen = new Pen(A(ArtworkTheme.Dim), Math.Max(1, 1.5f * s))) g.DrawLine(pen, x + cw - cut, cy, x + cw, cy + cut);
            float bw = 236 * s, bx = x + cw - pad - bw - 18 * s;
            Badge(g, "SSBU ARENA  ·  RANKED 1V1", x + pad, cy + 20 * s, ArtworkTheme.Accent, Color.White);
            string title = "THE COMPETITIVE LADDER";
            float titlePx = 30 * s, maxTitleW = bx - x - pad - 24 * s;
            while (titlePx > 18 * s && ArtworkTheme.TextWidth(g, title, FD(titlePx), 0.3f * s) > maxTitleW) titlePx -= 1 * s;
            ArtworkTheme.Label(g, title, FD(titlePx), A(ArtworkTheme.Text), x + pad, cy + 52 * s, 26 * s, 0.3f * s);
            ArtworkTheme.DrawText(g, "Tournament rules, one rating per account, tiers from Bronze to Grandmaster. Same account as OpenSmash.",
                FB(14 * s), A(ArtworkTheme.Muted), x + pad, cy + 84 * s, maxTitleW, 20 * s);
            bool windows = OperatingSystem.IsWindows();
            string label = c.ArenaExe != null ? "PLAY SSBU ARENA" : windows ? "GET SSBU ARENA" : "SSBU ARENA SITE";
            ArtworkTheme.ButtonAnimated(g, label, bx, cy + 16 * s, bw, 38 * s, s, primary: true, Hov("community:arena", _hover == "community:arena"), true, 13 * s, _alpha, press: Press("community:arena"));
            AddHit("community:arena", bx, cy + 16 * s, bw, 38 * s);
            ArtworkTheme.ButtonAnimated(g, "FULL LEADERBOARD  →", bx, cy + 60 * s, bw, 32 * s, s, primary: false, Hov("community:arena-board", _hover == "community:arena-board"), true, 12 * s, _alpha, press: Press("community:arena-board"));
            AddHit("community:arena-board", bx, cy + 60 * s, bw, 32 * s);
            PopFade(g, st);
        }

        // the list's head: the region filter and the columns
        float top = cy + ch + 54 * s;
        float cPos = x + 34 * s, cIcons = x + 52 * s, iconSz = 30 * s, cName = cIcons + 3 * (iconSz + 6 * s) + 14 * s;
        float cRating = x + cw - 14 * s, cWl = cRating - 120 * s, cTier = cWl - 110 * s - 190 * s;
        {
            var st = PushReveal(g, 1);
            var hf = FM(11 * s);
            float sp = 11 * s * 0.16f;
            string region = RankedBoard.RegionLabels[Math.Clamp(c.RankedRegion, 0, RankedBoard.Regions.Length - 1)];
            ArtworkTheme.Label(g, c.RankedRegion == 0 ? "TOP 10  ·  RANKED" : "TOP 10  ·  RANKED  ·  " + region, FM(12 * s, true), A(ArtworkTheme.Dim), x, top - 52 * s, 20 * s, 12 * s * 0.14f);
            float rx = x + cw;
            for (int i = RankedBoard.Regions.Length - 1; i >= 0; i--)
            {
                string id = "community:region:" + i.ToString(CultureInfo.InvariantCulture), text = RankedBoard.RegionLabels[i];
                var f = FM(11 * s, true);
                float w = ArtworkTheme.TextWidth(g, text, f, sp) + 26 * s, h = 26 * s;
                rx -= w;
                bool on = i == c.RankedRegion;
                float hv = Hov(id, _hover == id);
                using (var p = ArtworkTheme.Para(rx, top - 56 * s, w, h, 6 * s))
                {
                    if (on) { using var b = new SolidBrush(A(ArtworkTheme.Accent)); g.FillPath(b, p); }
                    else
                    {
                        using (var b = new SolidBrush(A(ArtworkTheme.Mix(ArtworkTheme.Bg2, ArtworkTheme.Bg3, hv)))) g.FillPath(b, p);
                        using (var pen = new Pen(A(ArtworkTheme.Line), Math.Max(1, s))) g.DrawPath(pen, p);
                    }
                }
                ArtworkTheme.Label(g, text, f, A(on ? Color.White : ArtworkTheme.Mix(ArtworkTheme.Muted, ArtworkTheme.Text, hv)), rx, top - 56 * s, h, sp, StringAlignment.Center, w);
                AddHit(id, rx, top - 56 * s, w, h);
                rx -= 8 * s;
            }
            ArtworkTheme.Label(g, "#", hf, A(ArtworkTheme.Dim), x, top - 22 * s, 20 * s, sp, StringAlignment.Far, cPos - x);
            ArtworkTheme.Label(g, "FIGHTERS", hf, A(ArtworkTheme.Dim), cIcons, top - 22 * s, 20 * s, sp);
            ArtworkTheme.Label(g, "PLAYER", hf, A(ArtworkTheme.Dim), cName, top - 22 * s, 20 * s, sp);
            ArtworkTheme.Label(g, "TIER", hf, A(ArtworkTheme.Dim), cTier, top - 22 * s, 20 * s, sp);
            ArtworkTheme.Label(g, "W / L", hf, A(ArtworkTheme.Dim), 0, top - 22 * s, 20 * s, sp, StringAlignment.Far, cWl);
            ArtworkTheme.Label(g, "RATING", hf, A(ArtworkTheme.Dim), 0, top - 22 * s, 20 * s, sp, StringAlignment.Far, cRating);
            HLine(g, x, x + cw, top);
            PopFade(g, st);
        }

        float bottom = H - StatusH * s - 30 * s;
        var rows = c.RankedRows ?? new List<RankedRow>();
        if (rows.Count == 0)
        {
            var st = PushReveal(g, 2);
            if (!c.RankedAsked)
                ArtworkTheme.Label(g, "Loading...", FB(15 * s), A(ArtworkTheme.Muted), x, top + 26 * s, 22 * s);
            else
            {
                // (quietly: an empty board today, or no answer - asked again in a few minutes)
                string big = c.RankedFailed ? "NOT AVAILABLE NOW." : "NO RANKED RESULTS YET.";
                var path = TextPath(big, 40 * s);
                var b = path.GetBounds();
                var ps = g.Save();
                g.TranslateTransform(x, top + 30 * s - b.Top);
                using (var pen = new Pen(A(ArtworkTheme.Dim), Math.Max(1, 1.5f * s))) g.DrawPath(pen, path);
                g.Restore(ps);
                ArtworkTheme.Label(g, c.RankedFailed ? "The ranked board could not be read. It is asked again in a few minutes."
                                                     : "Ranked 1v1 has just started. Players show here after their placement games in SSBU Arena.",
                    FB(15 * s), A(ArtworkTheme.Muted), x, top + 30 * s + b.Height + 14 * s, 22 * s);
            }
            PopFade(g, st);
            return;
        }

        float rowH = Math.Min(46 * s, Math.Max(24 * s, (bottom - top - 4 * s) / Math.Max(10, rows.Count)));
        for (int i = 0; i < rows.Count; i++)
        {
            var r = rows[i];
            float ry = top + i * rowH;
            if (ry + rowH > bottom + 1) break;
            var st = PushReveal(g, 2 + i);
            if (i % 2 == 1) using (var b = new SolidBrush(A(Color.FromArgb(70, ArtworkTheme.Bg2)))) g.FillRectangle(b, x, ry, cw, rowH);
            long pos = r.Rank > 0 ? r.Rank : i + 1;
            ArtworkTheme.Label(g, pos.ToString(CultureInfo.InvariantCulture), FM(15 * s, true), A(pos <= 3 ? ArtworkTheme.Accent : ArtworkTheme.Dim), x, ry, rowH, 0, StringAlignment.Far, cPos - x);
            float ix = cIcons, isz = Math.Min(iconSz, rowH - 10 * s);
            foreach (var f in r.Fighters.Take(3))
            {
                DrawFighter(g, f.Fighter, ix, ry + (rowH - isz) / 2, isz);
                ix += isz + 6 * s;
            }
            float nw = ArtworkTheme.Label(g, r.Username, FB(16 * s, true), A(ArtworkTheme.Text), cName, ry, rowH, 0, StringAlignment.Near, cTier - cName - 70 * s);
            ArtworkTheme.Label(g, "#" + r.Tag.ToString("0000", CultureInfo.InvariantCulture), FM(12 * s), A(ArtworkTheme.Dim), cName + nw + 4 * s, ry + 1 * s, rowH);
            var tc = TierColor(r);
            float em = 12 * s, ex = cTier, ey = ry + rowH / 2;
            using (var b = new SolidBrush(A(tc)))
                g.FillPolygon(b, new[] { new PointF(ex + em / 2, ey - em / 2 - 2 * s), new PointF(ex + em + 1 * s, ey), new PointF(ex + em / 2, ey + em / 2 + 2 * s), new PointF(ex - 1 * s, ey) });
            string tier = r.Tier != "" ? r.Tier.ToUpperInvariant() : "UNRANKED";
            ArtworkTheme.Label(g, tier, FM(12 * s, true), A(tc), ex + em + 10 * s, ry, rowH, 12 * s * 0.1f);
            ArtworkTheme.Label(g, r.Wins.ToString(CultureInfo.InvariantCulture) + " / " + r.Losses.ToString(CultureInfo.InvariantCulture), FM(13 * s), A(ArtworkTheme.Muted), 0, ry, rowH, 0, StringAlignment.Far, cWl);
            ArtworkTheme.Label(g, RankedBoard.RatingShown(r), FD(20 * s), A(ArtworkTheme.Text), 0, ry, rowH, 0.3f * s, StringAlignment.Far, cRating);
            HLine(g, x, x + cw, ry + rowH);
            PopFade(g, st);
        }
        ArtworkTheme.Label(g, "Stock icons by Max_Hamm", FB(11 * s), A(ArtworkTheme.Dim), 0, bottom + 6 * s, 16 * s, 0, StringAlignment.Far, x + cw);
    }
}
