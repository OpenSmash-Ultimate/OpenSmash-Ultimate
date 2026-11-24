// The home page's right column (main's LauncherForm.Community.cs / LauncherForm.Discord.cs): the player's Discord
// account (sign in, or avatar, name, best GSP and position; clicked, their GSP per fighter, the leaderboard name
// choice and sign out), the lobby's players online and open rooms, and the GSP leaderboard's top 10 with a link to
// the site's page. MainWindow.Discord.cs fills CommunityView; nothing is drawn until it enables the column.
// (mastery, OpenSmash.Core/Mastery.cs) With an account the GSP views become mastery points (the card's MASTERY per
// fighter: points, games, wins; no levels, no tiers) and the leaderboard becomes the "SSBU Arena · Competitive" card with
// the top mastery. OPENSMASH_MASTERY_UI=0: the GSP views as before.
using System.Drawing;
using System.Globalization;
using OpenSmash.Launcher;
using SkiaSharp;
namespace OpenSmash.Visuals;

public sealed class CommunityView
{
    public bool Enabled, HasApp, Busy, Open, Shown, Asked, Down;
    public string Error = "";
    public DiscordToken? Account;
    // (v0.5.0) the account (MainWindow.Account.cs): with a service in the build it replaces the Discord token above
    public bool AccountMode, AccountPage;
    public OpenSmash.Core.Profile? Me;
    public string? BoardId;   // the account's row on the leaderboard, as the lobby's /gsp/me gave it
    public bool SignedIn => AccountMode ? Me != null : Account != null;
    public string? ShownName => AccountMode ? Me?.Display : Account?.Shown;
    public byte[]? Avatar;
    public int Rank, Ranked;
    public Dictionary<int, int> FighterRanks = new();
    public List<FighterGsp> Records = new();
    public LobbyStatus? Lobby;
    public List<BoardRow> Top = new();
    public BoardRow? Mine;
    // (mastery: OpenSmash.Core.Mastery) the account's points summed over the regions, and the home's "SSBU Arena ·
    // Competitive" card with the top mastery in place of the GSP leaderboard. null with *Asked: no lobby has them yet.
    public OpenSmash.Core.MasteryMe? Mastery;
    public bool MasteryAsked, TopMasteryAsked;
    public List<OpenSmash.Core.MasteryRow>? TopMastery;
    public string? ArenaExe;   // Arena's launcher when installed (Windows only)
    // the COMPETITIVE page (CompetitiveArtwork.cs): SSBU Arena's ranked top 10 for a region (an index of
    // OpenSmash.Core.RankedBoard.Regions), filled by MainWindow.Discord.cs; null with RankedAsked: no answer yet
    public List<OpenSmash.Core.RankedRow>? RankedRows;
    public bool RankedAsked, RankedFailed;
    public int RankedRegion;
    // a fighter's stock icon (PNG bytes, OpenSmash.Core.Fighters) once downloaded; null meanwhile or without one
    public Func<string, byte[]?>? IconBytes;

    // One answer of CommunityFeed.Fetch: a lobby that answers nothing is offline, the last numbers kept.
    public void Apply(LobbyStatus? status, string? board, string accountHash)
    {
        Asked = true;
        Down = status == null && board == null;
        if (status != null) Lobby = status;
        if (board != null && CommunityFeed.ParseBoard(board) is { } parsed)
        {
            Top = parsed.Top;
            Ranked = parsed.Ranked;
            Mine = CommunityFeed.Mine(board, accountHash);
        }
    }

    public bool Up => Lobby != null && !Down;
    public string Headline => !Asked ? "ASKING THE LOBBY..." : !Up ? "LOBBY OFFLINE" : "ONLINE NOW";
}

public sealed partial class ClassicSurface
{
    public CommunityView Community { get; } = new();
    byte[]? _avatarBytes;
    Bitmap? _avatar;

    Bitmap? AvatarImage()
    {
        if (!ReferenceEquals(_avatarBytes, Community.Avatar))
        {
            _avatar?.Dispose(); _avatar = null; _avatarBytes = Community.Avatar;
            if (_avatarBytes != null && SKBitmap.Decode(_avatarBytes) is { } decoded) _avatar = new Bitmap(decoded);
        }
        return _avatar;
    }

    void PaintCommunity(Graphics g, int W, int H)
    {
        var c = Community;
        if (!c.Enabled) return;
        float s = _s, t = (float)(Now - _heroAt);
        float w = Math.Min(300 * s, W - 1032 * s), x = W - 32 * s - w, y = 128 * s;
        if (w < 220 * s) return;   // (a window too narrow for it)
        var st = PushFade(g, Up(t, 1.3f), 24 * s);
        void Panel(float py, float ph)
        {
            using var p = ArtworkTheme.Para(x, py, w, ph, 8 * s);
            using (var b = new SolidBrush(A(Color.FromArgb(200, ArtworkTheme.Bg2)))) g.FillPath(b, p);
            using (var pen = new Pen(A(ArtworkTheme.Line), Math.Max(1, s))) g.DrawPath(pen, p);
        }
        void Avatar(float ax, float ay, float a)
        {
            using var clip = new GraphicsPath();
            clip.AddEllipse(new RectangleF(ax, ay, a, a));
            if (AvatarImage() is { } img) { var gs = g.Save(); g.SetClip(clip, CombineMode.Intersect); g.DrawImage(img, ax, ay, a, a); g.Restore(gs); }
            else
            {
                using (var b = new SolidBrush(A(c.SignedIn ? Color.FromArgb(255, 88, 101, 242) : ArtworkTheme.Bg3))) g.FillPath(b, clip);
                // (an account has no picture here: its name's first letter, as the WinForms launcher's header)
                if (c.AccountMode && c.Me?.Username is { Length: > 0 } un)
                    ArtworkTheme.Label(g, un[..1].ToUpperInvariant(), FB(a * .45f, true), A(Color.White), ax, ay + a * .22f, a * .56f, 0, StringAlignment.Center, a);
            }
        }
        string N(long v) => v.ToString("N0", CultureInfo.InvariantCulture);
        float pad = 14 * s, ix = x + pad, iw = w - 2 * pad;
        bool mastery = OpenSmash.Core.Mastery.UiOn;   // (OPENSMASH_MASTERY_UI=0: the GSP views as before)

        // the account
        if (c.AccountMode || c.HasApp || c.Mine != null)
        {
            float ph = 64 * s;
            Panel(y, ph);
            if (!c.SignedIn && (c.HasApp || c.AccountMode))
            {
                ArtworkTheme.Label(g, c.Error != "" ? c.Error : c.AccountMode ? "SIGN IN TO PLAY ONLINE" : "PLAY ONLINE AS YOURSELF", FM(10 * s, true), A(c.Error != "" ? ArtworkTheme.Bad : ArtworkTheme.Dim), ix, y + 9 * s, 14 * s, 10 * s * 0.12f);
                // (v0.5.0: the accounts site offers Discord and e-mail; the official lobbies let in signed-in players only)
                string label = c.Busy ? "FINISH IN THE BROWSER  \u00D7" : c.AccountMode ? "SIGN IN" : "SIGN IN WITH DISCORD";
                string bid = c.Busy ? "community:cancel" : "community:signin";
                ArtworkTheme.ButtonAnimated(g, label, ix, y + 27 * s, iw, 28 * s, s, primary: true, Hov(bid, _hover == bid), true, 11 * s, _alpha, press: Press(bid));
                AddHit(bid, ix, y + 27 * s, iw, 28 * s);
            }
            else
            {
                float a = 40 * s, ay = y + (ph - a) / 2;
                Avatar(ix, ay, a);
                float tx = ix + a + 10 * s, tw = iw - a - 10 * s;
                // (mastery: the account's place on the accounts service's board when it has one; GSP: the lobby's)
                int rank = mastery && c.AccountMode ? (c.Mastery?.Rank is { Pos: > 0 and <= 99999 } mr ? (int)mr.Pos : 0) : c.Mine?.Rank > 0 ? c.Mine.Rank : c.Rank;
                string rankText = rank > 0 ? "#" + rank.ToString(CultureInfo.InvariantCulture) : "";
                float rw = rankText != "" ? ArtworkTheme.TextWidth(g, rankText, FD(22 * s), 0, false) + 8 * s : 0;
                float hv = Hov("community:me", c.SignedIn && _hover == "community:me");
                ArtworkTheme.Label(g, Fit(g, c.ShownName ?? "You", FB(15 * s, true), tw - rw), FB(15 * s, true), A(ArtworkTheme.Mix(ArtworkTheme.Text, ArtworkTheme.Accent, hv * .5f)), tx, ay, 20 * s);
                uint best = c.Mine?.Gsp ?? (c.Records.Count > 0 ? c.Records[0].Gsp : 0u);
                int fighter = c.Mine?.Fighter ?? (c.Records.Count > 0 ? c.Records[0].Fighter : -1);
                string line = best > 0 ? $"{N(best)} GSP  \u00B7  {DiscordAccount.FighterName(fighter)}" : "No GSP yet: play Quickplay online";
                if (mastery && c.AccountMode)
                {
                    // (mastery: the account's total and its first fighter; with the place in the corner, the fighter is left
                    // out when the line would not fit beside it: the total and the place matter more)
                    line =c.Mastery is { } m && m.Fighters.Count > 0
                        ? UiLocale.Format("{0} mastery points  \u00B7  {1}", N(m.Total), OpenSmash.Core.Mastery.FighterName(m.Fighters[0].Fighter))
                        : c.MasteryAsked && c.Mastery == null ? "Mastery points: not available yet" : "No mastery points yet: play online";
                    if (rankText != "" && c.Mastery is { } m2 && m2.Fighters.Count > 0 && ArtworkTheme.TextWidth(g, line, FB(11 * s), 0, false) > tw - rw)
                    {
                        string tpl = UiLocale.Text("{0} mastery points  \u00B7  {1}");
                        int cut = tpl.IndexOf("  \u00B7  ", StringComparison.Ordinal);
                        if (cut > 0 && tpl.IndexOf("{0}", StringComparison.Ordinal) < cut && tpl.IndexOf("{1}", StringComparison.Ordinal) > cut)
                            line = string.Format(CultureInfo.InvariantCulture, tpl[..cut], N(m2.Total));
                    }
                }
                ArtworkTheme.Label(g, Fit(g, line, FB(11 * s), tw - rw), FB(11 * s), A(ArtworkTheme.Muted), tx, ay + 21 * s, 16 * s);
                if (rankText != "") ArtworkTheme.Label(g, rankText, FD(22 * s), A(ArtworkTheme.Accent), x, y + 12 * s, 36 * s, 0, StringAlignment.Far, w - pad);
                if (c.SignedIn) AddHit("community:me", x, y, w, ph);
            }
            y += ph + 8 * s;
        }

        // the account card (in place of the lobby panels while open)
        if (c.Open && c.SignedIn)
        {
            bool showMastery = mastery && c.AccountMode;
            var rows = c.Records.Take(10).ToList();
            var mrows = showMastery && c.Mastery != null ? c.Mastery.Fighters.Take(8).ToList() : new List<OpenSmash.Core.MasteryFighter>();
            int count = showMastery ? mrows.Count : rows.Count;
            bool manage = c.AccountMode && c.AccountPage;   // (v0.5.0: the account page, where the name and sign-ins are changed)
            float rowH = 19 * s, ph = 30 * s + (showMastery ? 16 * s + 20 * s : 0) + Math.Max(1, count) * rowH + 14 * s + 30 * s + 8 * s + 30 * s + (manage ? 38 * s : 0) + (c.Error != "" ? 20 * s : 0) + 14 * s;
            // (mastery: a fighter's name gets the room left of its points and wraps to a second line rather than being cut)
            float lineH = 14 * s;
            float widestPoints = mrows.Count == 0 ? 0 : mrows.Max(r => ArtworkTheme.TextWidth(g, N(r.Total), FM(12 * s, true), 0, false));
            float iconW = 15 * s + 6 * s;   // (each fighter's stock icon before its name)
            float nameW = iw - 96 * s - widestPoints - 10 * s - iconW;
            var names = mrows.Select(r => Wrap2(g, OpenSmash.Core.Mastery.FighterName(r.Fighter), FB(12 * s), nameW)).ToList();
            ph += names.Sum(n => n.Count - 1) * lineH;
            Panel(y, ph);
            float ry = y + 30 * s;
            if (showMastery)
            {
                // (mastery: per fighter the points summed over the regions with the largest veteran bonus, games, wins;
                // the account's total. Points only: no levels, no tiers)
                ArtworkTheme.Label(g, "MASTERY", FM(10 * s, true), A(ArtworkTheme.Dim), ix, y + 9 * s, 14 * s, 10 * s * 0.12f);
                if (c.Mastery != null)
                {
                    // (the place among all the accounts, when the accounts service gave one and it fits)
                    string total = UiLocale.Format("{0} POINTS IN ALL", N(c.Mastery.Total));
                    string withPlace = c.Mastery.Rank != null ? OpenSmash.Core.Mastery.Place(c.Mastery.Rank) + "  ·  " + total : total;
                    float headW = ArtworkTheme.TextWidth(g, UiLocale.Text("MASTERY"), FM(10 * s, true), 10 * s * 0.12f, false);
                    if (ArtworkTheme.TextWidth(g, withPlace, FM(10 * s, true), 0, false) > iw - headW - 12 * s) withPlace = total;
                    ArtworkTheme.Label(g, withPlace, FM(10 * s, true), A(ArtworkTheme.Accent), ix, y + 9 * s, 14 * s, 0, StringAlignment.Far, iw);
                }
                float cWins = iw, cGames = iw - 44 * s, cPoints = iw - 96 * s;
                ArtworkTheme.Label(g, "POINTS", FM(9 * s, true), A(ArtworkTheme.Dim), ix, ry, 14 * s, 0, StringAlignment.Far, cPoints);
                ArtworkTheme.Label(g, "GAMES", FM(9 * s, true), A(ArtworkTheme.Dim), ix, ry, 14 * s, 0, StringAlignment.Far, cGames);
                ArtworkTheme.Label(g, "WINS", FM(9 * s, true), A(ArtworkTheme.Dim), ix, ry, 14 * s, 0, StringAlignment.Far, cWins);
                ry += 16 * s;
                if (mrows.Count == 0)
                    ArtworkTheme.Label(g, !c.MasteryAsked ? "Loading..." : c.Mastery == null ? "Not available on the lobby yet." : "No mastery points yet.", FB(12 * s), A(ArtworkTheme.Muted), ix, ry, rowH);
                for (int i = 0; i < mrows.Count; i++)
                {
                    var r = mrows[i];
                    // (its stock icon, CompetitiveArtwork.cs, then the name)
                    DrawFighter(g, r.Fighter, ix, ry + (rowH - 15 * s) / 2, 15 * s);
                    for (int k = 0; k < names[i].Count; k++)
                        ArtworkTheme.Label(g, names[i][k], FB(12 * s), A(ArtworkTheme.Text), ix + iconW, ry + k * lineH, rowH);
                    ArtworkTheme.Label(g, N(r.Total), FM(12 * s, true), A(ArtworkTheme.Text), ix, ry, rowH, 0, StringAlignment.Far, cPoints);
                    ArtworkTheme.Label(g, N(r.Games), FM(11 * s), A(ArtworkTheme.Muted), ix, ry, rowH, 0, StringAlignment.Far, cGames);
                    ArtworkTheme.Label(g, N(r.Wins), FM(11 * s), A(ArtworkTheme.Muted), ix, ry, rowH, 0, StringAlignment.Far, cWins);
                    ry += rowH + (names[i].Count - 1) * lineH;
                }
                if (mrows.Count == 0) ry += rowH;
                ArtworkTheme.Label(g, Fit(g, UiLocale.Text("Points for playing, they only go up."), FB(10 * s), iw), FB(10 * s), A(ArtworkTheme.Dim), ix, ry + 2 * s, 16 * s);
                ry += 20 * s;
            }
            else
            {
                ArtworkTheme.Label(g, "GSP PER FIGHTER", FM(10 * s, true), A(ArtworkTheme.Dim), ix, y + 9 * s, 14 * s, 10 * s * 0.12f);
                if (rows.Count == 0) ArtworkTheme.Label(g, "No online matches yet.", FB(12 * s), A(ArtworkTheme.Muted), ix, ry, rowH);
                foreach (var r in rows)
                {
                    ArtworkTheme.Label(g, DiscordAccount.FighterName(r.Fighter), FB(12 * s), A(ArtworkTheme.Text), ix, ry, rowH);
                    string gs = N(r.Gsp);
                    if (c.FighterRanks.TryGetValue(r.Fighter, out int fr) && fr > 0) gs = $"#{fr}   " + gs;
                    ArtworkTheme.Label(g, gs, FM(12 * s, true), A(ArtworkTheme.Muted), ix, ry, rowH, 0, StringAlignment.Far, iw);
                    ry += rowH;
                }
            }
            ry += 14 * s;
            string shown = c.Shown ? "HIDE MY NAME ON THE BOARD" : "SHOW MY NAME ON THE BOARD";
            ArtworkTheme.ButtonAnimated(g, shown, ix, ry, iw, 30 * s, s, primary: !c.Shown, Hov("community:shown", _hover == "community:shown"), true, 10 * s, _alpha, press: Press("community:shown"));
            AddHit("community:shown", ix, ry, iw, 30 * s);
            ry += 38 * s;
            if (manage)
            {
                ArtworkTheme.ButtonAnimated(g, "MANAGE ACCOUNT", ix, ry, iw, 30 * s, s, primary: false, Hov("community:manage", _hover == "community:manage"), true, 11 * s, _alpha, press: Press("community:manage"));
                AddHit("community:manage", ix, ry, iw, 30 * s);
                ry += 38 * s;
            }
            ArtworkTheme.ButtonAnimated(g, "SIGN OUT", ix, ry, iw, 30 * s, s, primary: false, Hov("community:signout", _hover == "community:signout"), true, 11 * s, _alpha, press: Press("community:signout"));
            AddHit("community:signout", ix, ry, iw, 30 * s);
            if (c.Error != "") ArtworkTheme.Label(g, c.Error, FB(11 * s), A(ArtworkTheme.Bad), ix, ry + 34 * s, 16 * s);
            PopFade(g, st);
            return;
        }

        // players online now and open rooms
        {
            float ph = 54 * s;
            Panel(y, ph);
            Color dot = !c.Asked ? ArtworkTheme.Dim : c.Up ? ArtworkTheme.Ready : ArtworkTheme.Bad;
            using (var b = new SolidBrush(A(dot))) g.FillEllipse(b, ix, y + 13 * s, 7 * s, 7 * s);
            ArtworkTheme.Label(g, c.Headline, FM(10 * s, true), A(ArtworkTheme.Dim), ix + 14 * s, y + 9 * s, 14 * s, 10 * s * 0.12f);
            if (c.Lobby != null)
            {
                var l = c.Lobby;
                string players = l.Online.ToString(CultureInfo.InvariantCulture) + (l.Online == 1 ? " PLAYER" : " PLAYERS");
                string rooms = l.OpenRooms.ToString(CultureInfo.InvariantCulture) + (l.OpenRooms == 1 ? " OPEN ROOM" : " OPEN ROOMS");
                float pw = ArtworkTheme.Label(g, players, FB(15 * s, true), A(c.Up ? ArtworkTheme.Text : ArtworkTheme.Muted), ix, y + 27 * s, 20 * s);
                ArtworkTheme.Label(g, "  ·  " + rooms, FB(12 * s), A(ArtworkTheme.Muted), ix + pw, y + 28 * s, 20 * s);
            }
            else if (c.Asked)
                ArtworkTheme.Label(g, "Try again in a minute.", FB(12 * s), A(ArtworkTheme.Muted), ix, y + 28 * s, 20 * s);
            y += ph + 8 * s;
        }

        // (mastery) the top mastery in place of the GSP leaderboard: each row with its main fighter's icon, and the site's
        // full list below. SSBU Arena (its ranked, its launcher) has its own page: COMPETITIVE, CompetitiveArtwork.cs.
        if (mastery)
        {
            if (OpenSmash.Core.Mastery.TopOn)
            {
                float rowH = 23 * s, head = 30 * s, foot = 32 * s, isz = 18 * s;
                var rows = c.TopMastery ?? new List<OpenSmash.Core.MasteryRow>();
                float ph = head + Math.Max(1, rows.Count) * rowH + foot;
                Panel(y, ph);
                ArtworkTheme.Label(g, "TOP MASTERY", FM(10 * s, true), A(ArtworkTheme.Dim), ix, y + 9 * s, 14 * s, 10 * s * 0.12f);
                float cy = y + head;
                if (rows.Count == 0) ArtworkTheme.Label(g, !c.TopMasteryAsked ? "Loading..." : "Not available now.", FB(12 * s), A(ArtworkTheme.Muted), ix, cy, rowH);
                for (int i = 0; i < rows.Count; i++)
                {
                    var r = rows[i];
                    ArtworkTheme.Label(g, (i + 1).ToString(CultureInfo.InvariantCulture), FM(12 * s, true), A(i < 3 ? ArtworkTheme.Accent : ArtworkTheme.Dim), ix, cy, rowH, 0, StringAlignment.Far, 16 * s);
                    float nx = ix + 24 * s;
                    if (r.MainFighter != "") DrawFighter(g, r.MainFighter, nx, cy + (rowH - isz) / 2, isz);
                    nx += isz + 7 * s;
                    ArtworkTheme.Label(g, Fit(g, OpenSmash.Core.Mastery.Who(r), FB(12 * s), iw - (nx - ix) - 64 * s), FB(12 * s), A(ArtworkTheme.Text), nx, cy, rowH);
                    ArtworkTheme.Label(g, N(r.Total), FM(12 * s, true), A(ArtworkTheme.Muted), ix, cy, rowH, 0, StringAlignment.Far, iw);
                    cy += rowH;
                }
                // the site's full mastery list
                float ly = y + ph - foot + 4 * s, lh = 24 * s;
                float hv = Hov("community:board", _hover == "community:board");
                ArtworkTheme.Label(g, "FULL LEADERBOARD  →", FM(11 * s, true), A(ArtworkTheme.Mix(ArtworkTheme.Muted, ArtworkTheme.Accent, hv)), ix, ly, lh, 11 * s * 0.08f);
                AddHit("community:board", ix, ly, iw, lh);
            }
        }
        // the leaderboard's top 10
        else
        {
            float rowH = 19 * s, head = 30 * s, foot = 32 * s;
            float ph = head + Math.Max(1, c.Top.Count) * rowH + foot;
            Panel(y, ph);
            string title = c.Ranked > 0 ? $"GSP LEADERBOARD  ·  {N(c.Ranked)} RANKED" : "GSP LEADERBOARD";
            ArtworkTheme.Label(g, title, FM(10 * s, true), A(ArtworkTheme.Dim), ix, y + 9 * s, 14 * s, 10 * s * 0.12f);
            float ry = y + head;
            if (c.Top.Count == 0) ArtworkTheme.Label(g, c.Asked ? "Not available now." : "Loading...", FB(12 * s), A(ArtworkTheme.Muted), ix, ry, rowH);
            foreach (var r in c.Top)
            {
                bool me = c.Mine != null && r.Id == c.Mine.Id;
                ArtworkTheme.Label(g, r.Rank.ToString(CultureInfo.InvariantCulture), FM(12 * s, true), A(r.Rank <= 3 ? ArtworkTheme.Accent : ArtworkTheme.Dim), ix, ry, rowH, 0, StringAlignment.Far, 20 * s);
                ArtworkTheme.Label(g, Fit(g, CommunityFeed.Who(r), FB(12 * s, me), iw - 96 * s), FB(12 * s, me), A(me ? ArtworkTheme.Accent : ArtworkTheme.Text), ix + 28 * s, ry, rowH);
                ArtworkTheme.Label(g, N(r.Gsp), FM(12 * s, true), A(me ? ArtworkTheme.Accent : ArtworkTheme.Muted), ix, ry, rowH, 0, StringAlignment.Far, iw);
                ry += rowH;
            }
            float ly = y + ph - foot + 4 * s, lh = 24 * s;
            float hv = Hov("community:board", _hover == "community:board");
            ArtworkTheme.Label(g, "FULL LEADERBOARD  →", FM(11 * s, true), A(ArtworkTheme.Mix(ArtworkTheme.Muted, ArtworkTheme.Accent, hv)), ix, ly, lh, 11 * s * 0.08f);
            AddHit("community:board", ix, ly, iw, lh);
        }
        PopFade(g, st);
    }

    // A line cut with an ellipsis to fit (shown names are the players' own, up to 32 characters).
    /// <summary>text in at most two lines of width max (broken at a space; the second line is fitted only when even two
    /// lines are not enough), so a short sentence or name wraps instead of losing its end</summary>
    List<string> Wrap2(Graphics g, string text, Font f, float max)
    {
        if (ArtworkTheme.TextWidth(g, text, f, 0, false) <= max) return new List<string> { text };
        int cut = -1;
        for (int i = text.IndexOf(' '); i > 0; i = text.IndexOf(' ', i + 1))
        {
            if (ArtworkTheme.TextWidth(g, text[..i], f, 0, false) > max) break;
            cut = i;
        }
        if (cut < 0) return new List<string> { Fit(g, text, f, max) };
        return new List<string> { text[..cut], Fit(g, text[(cut + 1)..].TrimStart(), f, max) };
    }

    string Fit(Graphics g, string text, Font f, float max)
    {
        if (ArtworkTheme.TextWidth(g, text, f, 0, false) <= max) return text;
        while (text.Length > 1 && ArtworkTheme.TextWidth(g, text + "…", f, 0, false) > max) text = text[..^1];
        return text.TrimEnd() + "…";
    }
}
