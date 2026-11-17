// The home page's right column (main's LauncherForm.Community.cs / LauncherForm.Discord.cs): the player's Discord
// account (sign in, or avatar, name, best GSP and position; clicked, their GSP per fighter, the leaderboard name
// choice and sign out), the lobby's players online and open rooms, and the GSP leaderboard's top 10 with a link to
// the site's page. MainWindow.Discord.cs fills CommunityView; nothing is drawn until it enables the column.
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
    public byte[]? Avatar;
    public int Rank, Ranked;
    public Dictionary<int, int> FighterRanks = new();
    public List<FighterGsp> Records = new();
    public LobbyStatus? Lobby;
    public List<BoardRow> Top = new();
    public BoardRow? Mine;

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
            else using (var b = new SolidBrush(A(c.Account != null ? Color.FromArgb(255, 88, 101, 242) : ArtworkTheme.Bg3))) g.FillPath(b, clip);
        }
        string N(long v) => v.ToString("N0", CultureInfo.InvariantCulture);
        float pad = 14 * s, ix = x + pad, iw = w - 2 * pad;

        // the account
        if (c.HasApp || c.Mine != null)
        {
            float ph = 64 * s;
            Panel(y, ph);
            if (c.Account == null && c.HasApp)
            {
                ArtworkTheme.Label(g, c.Error != "" ? c.Error : "PLAY ONLINE AS YOURSELF", FM(10 * s, true), A(c.Error != "" ? ArtworkTheme.Bad : ArtworkTheme.Dim), ix, y + 9 * s, 14 * s, 10 * s * 0.12f);
                string label = c.Busy ? "FINISH IN THE BROWSER  \u00D7" : "SIGN IN WITH DISCORD";
                string bid = c.Busy ? "community:cancel" : "community:signin";
                ArtworkTheme.ButtonAnimated(g, label, ix, y + 27 * s, iw, 28 * s, s, primary: true, Hov(bid, _hover == bid), true, 11 * s, _alpha, press: Press(bid));
                AddHit(bid, ix, y + 27 * s, iw, 28 * s);
            }
            else
            {
                float a = 40 * s, ay = y + (ph - a) / 2;
                Avatar(ix, ay, a);
                float tx = ix + a + 10 * s, tw = iw - a - 10 * s;
                int rank = c.Mine?.Rank > 0 ? c.Mine.Rank : c.Rank;
                string rankText = rank > 0 ? "#" + rank.ToString(CultureInfo.InvariantCulture) : "";
                float rw = rankText != "" ? ArtworkTheme.TextWidth(g, rankText, FD(22 * s), 0, false) + 8 * s : 0;
                float hv = Hov("community:me", c.Account != null && _hover == "community:me");
                ArtworkTheme.Label(g, Fit(g, c.Account?.Shown ?? "You", FB(15 * s, true), tw - rw), FB(15 * s, true), A(ArtworkTheme.Mix(ArtworkTheme.Text, ArtworkTheme.Accent, hv * .5f)), tx, ay, 20 * s);
                uint best = c.Mine?.Gsp ?? (c.Records.Count > 0 ? c.Records[0].Gsp : 0u);
                int fighter = c.Mine?.Fighter ?? (c.Records.Count > 0 ? c.Records[0].Fighter : -1);
                string line = best > 0 ? $"{N(best)} GSP  \u00B7  {DiscordAccount.FighterName(fighter)}" : "No GSP yet: play Quickplay online";
                ArtworkTheme.Label(g, Fit(g, line, FB(11 * s), tw - rw), FB(11 * s), A(ArtworkTheme.Muted), tx, ay + 21 * s, 16 * s);
                if (rankText != "") ArtworkTheme.Label(g, rankText, FD(22 * s), A(ArtworkTheme.Accent), x, y + 12 * s, 36 * s, 0, StringAlignment.Far, w - pad);
                if (c.Account != null) AddHit("community:me", x, y, w, ph);
            }
            y += ph + 8 * s;
        }

        // the account card (in place of the lobby panels while open)
        if (c.Open && c.Account != null)
        {
            var rows = c.Records.Take(10).ToList();
            float rowH = 19 * s, ph = 30 * s + Math.Max(1, rows.Count) * rowH + 14 * s + 30 * s + 8 * s + 30 * s + (c.Error != "" ? 20 * s : 0) + 14 * s;
            Panel(y, ph);
            ArtworkTheme.Label(g, "GSP PER FIGHTER", FM(10 * s, true), A(ArtworkTheme.Dim), ix, y + 9 * s, 14 * s, 10 * s * 0.12f);
            float ry = y + 30 * s;
            if (rows.Count == 0) ArtworkTheme.Label(g, "No online matches yet.", FB(12 * s), A(ArtworkTheme.Muted), ix, ry, rowH);
            foreach (var r in rows)
            {
                ArtworkTheme.Label(g, DiscordAccount.FighterName(r.Fighter), FB(12 * s), A(ArtworkTheme.Text), ix, ry, rowH);
                string gs = N(r.Gsp);
                if (c.FighterRanks.TryGetValue(r.Fighter, out int fr) && fr > 0) gs = $"#{fr}   " + gs;
                ArtworkTheme.Label(g, gs, FM(12 * s, true), A(ArtworkTheme.Muted), ix, ry, rowH, 0, StringAlignment.Far, iw);
                ry += rowH;
            }
            ry += 14 * s;
            string shown = c.Shown ? "HIDE MY NAME ON THE BOARD" : "SHOW MY NAME ON THE BOARD";
            ArtworkTheme.ButtonAnimated(g, shown, ix, ry, iw, 30 * s, s, primary: !c.Shown, Hov("community:shown", _hover == "community:shown"), true, 10 * s, _alpha, press: Press("community:shown"));
            AddHit("community:shown", ix, ry, iw, 30 * s);
            ry += 38 * s;
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

        // the leaderboard's top 10
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
    string Fit(Graphics g, string text, Font f, float max)
    {
        if (ArtworkTheme.TextWidth(g, text, f, 0, false) <= max) return text;
        while (text.Length > 1 && ArtworkTheme.TextWidth(g, text + "…", f, 0, false) > max) text = text[..^1];
        return text.TrimEnd() + "…";
    }
}
