using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using OpenSmash.Core;

namespace OpenSmash.Launcher
{
    // The COMPETITIVE page (top menu, between MODS and SETTINGS): SSBU Arena's ranked 1v1, the competitive ladder of the
    // same account (OpenSmash itself has mastery points, no ranking). Its top 10 from the accounts service's public
    // ranked_board (OpenSmash.Core.RankedBoard; All / EU / NA East), each row with the player's fighters (Max_Hamm's stock
    // icons as ssbuarena.com serves them: OpenSmash.Core.Fighters), name#tag, tier (its colour) and rating; the main
    // button starts SSBU Arena when it is installed (its launcher writes %LOCALAPPDATA%\SSBU Arena\install_path.txt:
    // OpenSmash.Core.ArenaEdition), else opens its download page; "Full leaderboard" opens ssbuarena.com's. An empty board
    // or no answer is said quietly; the page asks again every minute while it is shown.
    // Also here: the fighter icons the home's top mastery and the account card draw.
    public partial class LauncherForm
    {
        List<RankedRow>? _rankedRows;
        bool _rankedAsked, _rankedFailed;
        int _rankedRegion;   // (an index of RankedBoard.Regions: 0 every region)
        int _rankedBusy;
        DateTime _rankedAt = DateTime.MinValue;
        readonly Dictionary<string, Image?> _fighterIcons = new();

        /// <summary>the COMPETITIVE page is offered (OPENSMASH_MASTERY_UI=0 or OPENSMASH_ARENA_CARD=0: not)</summary>
        static bool CompetitiveOn => Mastery.UiOn && Mastery.ArenaCardOn;

        async Task RankedRefresh(bool force = false)
        {
            if (!force && _rankedAsked && DateTime.UtcNow - _rankedAt < TimeSpan.FromSeconds(55)) return;
            if (Interlocked.Exchange(ref _rankedBusy, 1) == 1) return;
            try
            {
                int region = _rankedRegion;
                _arenaExe = await Task.Run(() => ArenaEdition.Find());
                var rows = await RankedBoard.TopAsync(_account, RankedBoard.Regions[region], 10);
                if (region != _rankedRegion) return;   // (another region was picked meanwhile: its own answer follows)
                _rankedFailed = rows == null;
                if (rows != null) _rankedRows = rows;
                _rankedAsked = true;
                _rankedAt = DateTime.UtcNow;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidOperationException) { _rankedFailed = true; _rankedAsked = true; }
            finally { Interlocked.Exchange(ref _rankedBusy, 0); }
            if (_view == "competitive") Invalidate();
        }

        bool ActivateCompetitiveHit(string id)
        {
            switch (id)
            {
                case "comp:arena":
                    Updater.Log(_repo, ArenaEdition.Open(url => OpenUrl(url)));
                    return true;
                case "comp:full":
                    OpenUrl(RankedBoard.LeaderboardPage);
                    return true;
            }
            if (id.StartsWith("comp:region:") && int.TryParse(id.Substring(12), out int r) && r >= 0 && r < RankedBoard.Regions.Length)
            {
                if (r != _rankedRegion) { _rankedRegion = r; _rankedRows = null; _rankedAsked = false; _ = RankedRefresh(force: true); }
                Invalidate();
                return true;
            }
            return false;
        }

        // ---- the fighter icons ---------------------------------------------------------------------------------------
        /// <summary>a fighter's icon (any spelling), once downloaded; null meanwhile or when there is none</summary>
        Image? FighterIcon(string? fighter)
        {
            string? folder = Fighters.IconFolder(fighter);
            if (folder == null) return null;
            if (_fighterIcons.TryGetValue(folder, out var img) && img != null) return img;
            byte[]? bytes = Fighters.Icon(_httpClient, fighter, () => { try { BeginInvoke(new Action(Invalidate)); } catch (InvalidOperationException) { } });
            if (bytes == null) return null;
            try
            {
                using var ms = new MemoryStream(bytes);
                using var raw = Image.FromStream(ms);
                img = new Bitmap(raw);
            }
            catch (Exception e) when (e is ArgumentException or OutOfMemoryException or ExternalException) { img = null; }
            _fighterIcons[folder] = img;
            return img;
        }

        /// <summary>a fighter's stock icon in a size x size square; without one, its initial on a disc</summary>
        void DrawFighter(Graphics g, string? fighter, float x, float y, float size)
        {
            if (string.IsNullOrEmpty(fighter)) return;
            if (FighterIcon(fighter) is { } img)
            {
                var im = g.InterpolationMode;
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                if (_alpha >= 0.999f) g.DrawImage(img, x, y, size, size);
                else
                {
                    using var ia = new System.Drawing.Imaging.ImageAttributes();
                    ia.SetColorMatrix(new System.Drawing.Imaging.ColorMatrix { Matrix33 = _alpha });
                    g.DrawImage(img, new Rectangle((int)x, (int)y, (int)size, (int)size), 0, 0, img.Width, img.Height, GraphicsUnit.Pixel, ia);
                }
                g.InterpolationMode = im;
                return;
            }
            using (var b = new SolidBrush(A(Theme.Bg3))) g.FillEllipse(b, x, y, size, size);
            string name = Fighters.Name(fighter);
            Theme.Label(g, name.Length > 0 ? name.Substring(0, 1).ToUpperInvariant() : "?", FM(size * 0.42f, true), A(Theme.Muted), x, y, size, 0, StringAlignment.Center, size);
        }

        static Color TierColor(RankedRow r)
        {
            string c = r.TierColor ?? "#8C9AAD";
            try { return Color.FromArgb(255, Convert.ToInt32(c.Substring(1, 2), 16), Convert.ToInt32(c.Substring(3, 2), 16), Convert.ToInt32(c.Substring(5, 2), 16)); }
            catch (FormatException) { return Color.FromArgb(255, 0x8C, 0x9A, 0xAD); }
        }

        // ---- the page ------------------------------------------------------------------------------------------------
        void PaintCompetitive(Graphics g, int W, int H)
        {
            float s = _s, x = Gutter * s, cw = W - 2 * x;
            PaintSectionHead(g, "03 / COMPETITIVE", "COMPETITIVE");

            // the SSBU Arena card: what it is, the main button and the full leaderboard
            float cy = ContentTop * s, ch = 108 * s, cut = 22 * s, pad = 24 * s;
            {
                var st = PushReveal(g, 0);
                using (var path = Theme.CutCorner(x, cy, cw, ch, cut))
                {
                    using (var b = new SolidBrush(A(Theme.Bg2))) g.FillPath(b, path);
                    using (var pen = new Pen(A(Theme.Line), Math.Max(1, s))) g.DrawPath(pen, path);
                }
                using (var pen = new Pen(A(Theme.Dim), Math.Max(1, 1.5f * s))) g.DrawLine(pen, x + cw - cut, cy, x + cw, cy + cut);
                float bw = 236 * s, bx = x + cw - pad - bw - 18 * s;
                Badge(g, "SSBU ARENA  ·  RANKED 1V1", x + pad, cy + 20 * s, Theme.Accent, Color.White);
                string title = "THE COMPETITIVE LADDER";
                float titlePx = 30 * s, maxTitleW = bx - x - pad - 24 * s;
                while (titlePx > 18 * s && Theme.TextWidth(g, title, FD(titlePx), 0.3f * s) > maxTitleW) titlePx -= 1 * s;
                Theme.Label(g, title, FD(titlePx), A(Theme.Text), x + pad, cy + 52 * s, 26 * s, 0.3f * s);
                Theme.DrawText(g, "Tournament rules, one rating per account, tiers from Bronze to Grandmaster. Same account as OpenSmash.",
                    FB(14 * s), A(Theme.Muted), x + pad, cy + 84 * s, maxTitleW, 20 * s);
                string label = _arenaExe != null ? "PLAY SSBU ARENA" : "GET SSBU ARENA";
                Theme.ButtonAnimated(g, label, bx, cy + 16 * s, bw, 38 * s, s, primary: true, Hov("comp:arena", _hover == "comp:arena"), true, 13 * s, _alpha, press: Press("comp:arena"));
                AddHit("comp:arena", bx, cy + 16 * s, bw, 38 * s);
                Theme.ButtonAnimated(g, "FULL LEADERBOARD  →", bx, cy + 60 * s, bw, 32 * s, s, primary: false, Hov("comp:full", _hover == "comp:full"), true, 12 * s, _alpha, press: Press("comp:full"));
                AddHit("comp:full", bx, cy + 60 * s, bw, 32 * s);
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
                string region = RankedBoard.RegionLabels[_rankedRegion];
                Theme.Label(g, _rankedRegion == 0 ? "TOP 10  ·  RANKED" : "TOP 10  ·  RANKED  ·  " + region, FM(12 * s, true), A(Theme.Dim), x, top - 52 * s, 20 * s, 12 * s * 0.14f);
                // (All / EU / NA East: the board's region filter)
                float rx = x + cw;
                for (int i = RankedBoard.Regions.Length - 1; i >= 0; i--)
                {
                    string id = "comp:region:" + i.ToString(CultureInfo.InvariantCulture), text = RankedBoard.RegionLabels[i];
                    var f = FM(11 * s, true);
                    float w = Theme.TextWidth(g, text, f, sp) + 26 * s, h = 26 * s;
                    rx -= w;
                    bool on = i == _rankedRegion;
                    float hv = Hov(id, _hover == id);
                    using (var p = Theme.Para(rx, top - 56 * s, w, h, 6 * s))
                    {
                        if (on) { using var b = new SolidBrush(A(Theme.Accent)); g.FillPath(b, p); }
                        else
                        {
                            using (var b = new SolidBrush(A(Theme.Mix(Theme.Bg2, Theme.Bg3, hv)))) g.FillPath(b, p);
                            using (var pen = new Pen(A(Theme.Line), Math.Max(1, s))) g.DrawPath(pen, p);
                        }
                    }
                    Theme.Label(g, text, f, A(on ? Color.White : Theme.Mix(Theme.Muted, Theme.Text, hv)), rx, top - 56 * s, h, sp, StringAlignment.Center, w);
                    AddHit(id, rx, top - 56 * s, w, h);
                    rx -= 8 * s;
                }
                Theme.Label(g, "#", hf, A(Theme.Dim), x, top - 22 * s, 20 * s, sp, StringAlignment.Far, cPos - x);
                Theme.Label(g, "FIGHTERS", hf, A(Theme.Dim), cIcons, top - 22 * s, 20 * s, sp);
                Theme.Label(g, "PLAYER", hf, A(Theme.Dim), cName, top - 22 * s, 20 * s, sp);
                Theme.Label(g, "TIER", hf, A(Theme.Dim), cTier, top - 22 * s, 20 * s, sp);
                Theme.Label(g, "W / L", hf, A(Theme.Dim), 0, top - 22 * s, 20 * s, sp, StringAlignment.Far, cWl);
                Theme.Label(g, "RATING", hf, A(Theme.Dim), 0, top - 22 * s, 20 * s, sp, StringAlignment.Far, cRating);
                HLine(g, x, x + cw, top);
                PopFade(g, st);
            }

            float bottom = H - StatusH * s - 30 * s;
            var rows = _rankedRows ?? new List<RankedRow>();
            if (rows.Count == 0)
            {
                var st = PushReveal(g, 2);
                if (!_rankedAsked)
                    Theme.Label(g, "Loading...", FB(15 * s), A(Theme.Muted), x, top + 26 * s, 22 * s);
                else
                {
                    // (quietly: an empty board today, or no answer - asked again in a minute)
                    string big = _rankedFailed ? "NOT AVAILABLE NOW." : "NO RANKED RESULTS YET.";
                    var path = TextPath(big, 40 * s);
                    var b = path.GetBounds();
                    var ps = g.Save();
                    g.TranslateTransform(x, top + 30 * s - b.Top);
                    using (var pen = new Pen(A(Theme.Dim), Math.Max(1, 1.5f * s))) g.DrawPath(pen, path);
                    g.Restore(ps);
                    Theme.Label(g, _rankedFailed ? "The ranked board could not be read. It is asked again in a minute."
                                                 : "Ranked 1v1 has just started. Players show here after their placement games in SSBU Arena.",
                        FB(15 * s), A(Theme.Muted), x, top + 30 * s + b.Height + 14 * s, 22 * s);
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
                if (i % 2 == 1) using (var b = new SolidBrush(A(Color.FromArgb(70, Theme.Bg2)))) g.FillRectangle(b, x, ry, cw, rowH);
                long pos = r.Rank > 0 ? r.Rank : i + 1;
                Theme.Label(g, pos.ToString(CultureInfo.InvariantCulture), FM(15 * s, true), A(pos <= 3 ? Theme.Accent : Theme.Dim), x, ry, rowH, 0, StringAlignment.Far, cPos - x);
                float ix = cIcons, isz = Math.Min(iconSz, rowH - 10 * s);
                foreach (var f in r.Fighters.Take(3))
                {
                    DrawFighter(g, f.Fighter, ix, ry + (rowH - isz) / 2, isz);
                    ix += isz + 6 * s;
                }
                float nw = Theme.Label(g, r.Username, FB(16 * s, true), A(Theme.Text), cName, ry, rowH, 0, StringAlignment.Near, cTier - cName - 70 * s);
                Theme.Label(g, "#" + r.Tag.ToString("0000", CultureInfo.InvariantCulture), FM(12 * s), A(Theme.Dim), cName + nw + 4 * s, ry + 1 * s, rowH);
                // the tier: its emblem (a diamond in its colour) and its name
                var tc = TierColor(r);
                float em = 12 * s, ex = cTier, ey = ry + rowH / 2;
                using (var b = new SolidBrush(A(tc)))
                    g.FillPolygon(b, new[] { new PointF(ex + em / 2, ey - em / 2 - 2 * s), new PointF(ex + em + 1 * s, ey), new PointF(ex + em / 2, ey + em / 2 + 2 * s), new PointF(ex - 1 * s, ey) });
                string tier = r.Tier != "" ? r.Tier.ToUpperInvariant() : "UNRANKED";
                Theme.Label(g, tier, FM(12 * s, true), A(tc), ex + em + 10 * s, ry, rowH, 12 * s * 0.1f);
                Theme.Label(g, r.Wins.ToString(CultureInfo.InvariantCulture) + " / " + r.Losses.ToString(CultureInfo.InvariantCulture), FM(13 * s), A(Theme.Muted), 0, ry, rowH, 0, StringAlignment.Far, cWl);
                Theme.Label(g, RankedBoard.RatingShown(r), FD(20 * s), A(Theme.Text), 0, ry, rowH, 0.3f * s, StringAlignment.Far, cRating);
                HLine(g, x, x + cw, ry + rowH);
                PopFade(g, st);
            }
            Theme.Label(g, "Stock icons by Max_Hamm", FB(11 * s), A(Theme.Dim), 0, bottom + 6 * s, 16 * s, 0, StringAlignment.Far, x + cw);
        }
    }
}
