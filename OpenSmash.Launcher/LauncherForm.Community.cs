using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace OpenSmash.Launcher
{
    // The home page's right column: the player's Discord account (sign in, or their name, avatar, GSP and position),
    // the lobby's players online now and open rooms, and the GSP leaderboard's top 10 with a link to the site's page
    // (CommunityFeed.cs). Asked again every minute; a lobby that does not answer shows as offline, the last numbers
    // kept. And the launcher's own Discord Rich Presence, "In the launcher" (DiscordPresence.cs).
    public partial class LauncherForm
    {
        LobbyStatus? _lobby;
        List<BoardRow> _top = new();
        BoardRow? _mine;
        int _ranked;
        bool _lobbyAsked, _lobbyDown;
        System.Windows.Forms.Timer? _communityTimer, _presenceTimer;
        DiscordPresence? _presence;
        int _communityBusy;

        void CommunityInit()
        {
            _ = CommunityRefresh();
            _communityTimer = new System.Windows.Forms.Timer { Interval = (int)CommunityFeed.Every.TotalMilliseconds };
            _communityTimer.Tick += (_, _) => _ = CommunityRefresh();
            _communityTimer.Start();
            PresenceStart();
        }

        async Task CommunityRefresh()
        {
            if (Interlocked.Exchange(ref _communityBusy, 1) == 1) return;
            try
            {
                var (status, board) = await CommunityFeed.Fetch(_httpClient, CancellationToken.None);
                _lobbyAsked = true;
                _lobbyDown = status == null && board == null;
                if (status != null) _lobby = status;
                if (board != null && CommunityFeed.ParseBoard(board) is { } parsed)
                {
                    _top = parsed.Top;
                    _ranked = parsed.Ranked;
                    string nsa = await Task.Run(() => DiscordAccount.NsaId(_repo));
                    _mine = CommunityFeed.Mine(board, nsa == "" ? "" : DiscordAccount.PlayerId(nsa));
                }
            }
            catch (Exception e) when (e is System.IO.IOException or UnauthorizedAccessException) { }
            finally { Interlocked.Exchange(ref _communityBusy, 0); }
            if (_view == "home") Invalidate();
        }

        // ---- Rich Presence: the launcher's, until the game starts ---------------------------------------------------
        void PresenceStart()
        {
            if (_presence != null) return;
            string app = DiscordAccount.AppId(_repo);
            if (app == "") return;
            try { _presence = DiscordPresence.Start(_buildDir, app); }
            catch (Exception e) when (e is DllNotFoundException or BadImageFormatException or System.Runtime.InteropServices.SEHException) { _presence = null; }
            if (_presence == null) return;
            _presence.Show("In the launcher");
            _presenceTimer = new System.Windows.Forms.Timer { Interval = 100 };
            _presenceTimer.Tick += (_, _) =>
            {
                try { _presence?.Tick(); }
                catch (System.Runtime.InteropServices.SEHException) { PresenceStop(); }
            };
            _presenceTimer.Start();
        }

        // (before the game starts: its own Rich Presence takes over; and when the window closes)
        void PresenceStop()
        {
            _presenceTimer?.Stop();
            _presenceTimer?.Dispose();
            _presenceTimer = null;
            _presence?.Dispose();
            _presence = null;
        }

        void CommunityStop()
        {
            _communityTimer?.Stop();
            _communityTimer?.Dispose();
            _communityTimer = null;
            PresenceStop();
        }

        bool ActivateCommunityHit(string id)
        {
            switch (id)
            {
                case "community:signin": DiscordSignIn(); return true;
                case "community:cancel": _discordCancel?.Cancel(); return true;
                case "community:me": _discordOpen = !_discordOpen; if (_discordOpen) _ = DiscordLoadGsp(); Invalidate(); return true;
                case "community:board":
                    try { Process.Start(new ProcessStartInfo(CommunityFeed.SitePage) { UseShellExecute = true }); }
                    catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException) { }
                    return true;
            }
            return false;
        }

        // The column right of the hero, from the kicker's line down to above the chips.
        void PaintCommunity(Graphics g, int W, int H)
        {
            float s = _s, t = (float)(Now - _heroAt);
            float w = 340 * s, x = W - 40 * s - w, y = 128 * s;
            if (x < 860 * s) return;   // (a window too narrow for it)
            var st = PushFade(g, Up(t, 1.3f), 24 * s);
            void Panel(float py, float ph)
            {
                using var p = Theme.Para(x, py, w, ph, 8 * s);
                using (var b = new SolidBrush(A(Color.FromArgb(200, Theme.Bg2)))) g.FillPath(b, p);
                using (var pen = new Pen(A(Theme.Line), Math.Max(1, s))) g.DrawPath(pen, p);
            }
            float pad = 16 * s, ix = x + pad, iw = w - 2 * pad;

            // the account
            if (_discordHasApp || _mine != null)
            {
                float ph = 72 * s;
                Panel(y, ph);
                if (_discord == null && _discordHasApp)
                {
                    Theme.Label(g, "PLAY ONLINE AS YOURSELF", FM(11 * s, true), A(Theme.Dim), ix, y + 12 * s, 14 * s, 11 * s * 0.12f);
                    string label = _discordBusy ? "FINISH IN THE BROWSER  ×" : "SIGN IN WITH DISCORD";
                    string bid = _discordBusy ? "community:cancel" : "community:signin";
                    Theme.ButtonAnimated(g, label, ix, y + 30 * s, iw, 32 * s, s, primary: true, Hov(bid, _hover == bid), true, 12 * s, _alpha, press: Press(bid));
                    AddHit(bid, ix, y + 30 * s, iw, 32 * s);
                }
                else
                {
                    float a = 44 * s, ay = y + (ph - a) / 2;
                    using (var clip = new GraphicsPath())
                    {
                        clip.AddEllipse(ix, ay, a, a);
                        if (_discordAvatar != null) { var gs = g.Save(); g.SetClip(clip); g.DrawImage(_discordAvatar, ix, ay, a, a); g.Restore(gs); }
                        else using (var b = new SolidBrush(A(_discord != null ? Color.FromArgb(255, 88, 101, 242) : Theme.Bg3))) g.FillPath(b, clip);
                    }
                    float tx = ix + a + 12 * s, tw = iw - a - 12 * s;
                    string who = _discord?.Shown ?? "You";
                    Theme.Label(g, who, FB(17 * s, true), A(Theme.Text), tx, ay - 1 * s, 24 * s, 0, StringAlignment.Near, tw);
                    int rank = _mine?.Rank > 0 ? _mine.Rank : _discordRank;
                    uint best = _mine?.Gsp ?? (_discordRecords.Count > 0 ? _discordRecords[0].Gsp : 0u);
                    int fighter = _mine?.Fighter ?? (_discordRecords.Count > 0 ? _discordRecords[0].Fighter : -1);
                    string line = best > 0
                        ? UiLocale.Format("{0} GSP  ·  {1}", best.ToString("N0", CultureInfo.InvariantCulture), DiscordAccount.FighterName(fighter))
                        : UiLocale.Text("No GSP yet: play Quickplay online");
                    Theme.Label(g, line, FB(13 * s), A(Theme.Muted), tx, ay + 24 * s, 18 * s, 0, StringAlignment.Near, tw - 60 * s);
                    if (rank > 0)
                        Theme.Label(g, "#" + rank.ToString(CultureInfo.InvariantCulture), FD(26 * s), A(Theme.Accent), x, y + 14 * s, 40 * s, 0, StringAlignment.Far, w - pad);
                    if (_discord != null) AddHit("community:me", x, y, w, ph);
                }
                y += ph + 10 * s;
            }

            // players online now and open rooms
            {
                float ph = 58 * s;
                Panel(y, ph);
                bool up = _lobby != null && !_lobbyDown;
                Color dot = !_lobbyAsked ? Theme.Dim : up ? Theme.Ready : Theme.Bad;
                using (var b = new SolidBrush(A(dot))) g.FillEllipse(b, ix, y + 16 * s, 8 * s, 8 * s);
                string head = !_lobbyAsked ? "ASKING THE LOBBY..." : !up ? "LOBBY OFFLINE" : "ONLINE NOW";
                Theme.Label(g, head, FM(11 * s, true), A(Theme.Dim), ix + 16 * s, y + 12 * s, 16 * s, 11 * s * 0.12f);
                if (_lobby != null)
                {
                    string players = UiLocale.Format(_lobby.Online == 1 ? "{0} PLAYER" : "{0} PLAYERS", _lobby.Online.ToString(CultureInfo.InvariantCulture));
                    string rooms = UiLocale.Format(_lobby.OpenRooms == 1 ? "{0} OPEN ROOM" : "{0} OPEN ROOMS", _lobby.OpenRooms.ToString(CultureInfo.InvariantCulture));
                    float pw = Theme.Label(g, players, FB(18 * s, true), A(up ? Theme.Text : Theme.Muted), ix, y + 30 * s, 22 * s);
                    Theme.Label(g, "  ·  " + rooms, FB(15 * s), A(Theme.Muted), ix + pw, y + 31 * s, 22 * s);
                }
                else if (_lobbyAsked)
                    Theme.Label(g, "Try again in a minute.", FB(14 * s), A(Theme.Muted), ix, y + 31 * s, 22 * s);
                y += ph + 10 * s;
            }

            // the leaderboard's top 10
            {
                float rowH = 21 * s, head = 34 * s, foot = 36 * s;
                int n = Math.Max(1, _top.Count);
                float ph = head + n * rowH + foot;
                Panel(y, ph);
                string title = _ranked > 0 ? UiLocale.Format("GSP LEADERBOARD  ·  {0} RANKED", _ranked.ToString("N0", CultureInfo.InvariantCulture)) : "GSP LEADERBOARD";
                Theme.Label(g, title, FM(11 * s, true), A(Theme.Dim), ix, y + 12 * s, 16 * s, 11 * s * 0.12f);
                float ry = y + head;
                if (_top.Count == 0)
                    Theme.Label(g, _lobbyAsked ? "Not available now." : "Loading...", FB(13 * s), A(Theme.Muted), ix, ry, rowH);
                foreach (var r in _top)
                {
                    bool me = _mine != null && r.Id == _mine.Id;
                    Color c = me ? Theme.Accent : Theme.Text;
                    Theme.Label(g, r.Rank.ToString(CultureInfo.InvariantCulture), FM(13 * s, true), A(r.Rank <= 3 ? Theme.Accent : Theme.Dim), ix, ry, rowH, 0, StringAlignment.Far, 22 * s);
                    Theme.Label(g, CommunityFeed.Who(r), FB(13 * s, me), A(c), ix + 32 * s, ry, rowH, 0, StringAlignment.Near, iw - 110 * s);
                    Theme.Label(g, r.Gsp.ToString("N0", CultureInfo.InvariantCulture), FM(13 * s, true), A(me ? Theme.Accent : Theme.Muted), ix, ry, rowH, 0, StringAlignment.Far, iw);
                    ry += rowH;
                }
                float ly = y + ph - foot + 4 * s, lh = 26 * s;
                float hv = Hov("community:board", _hover == "community:board");
                Theme.Label(g, "FULL LEADERBOARD  →", FM(12 * s, true), A(Theme.Mix(Theme.Muted, Theme.Accent, hv)), ix, ly, lh, 12 * s * 0.08f);
                AddHit("community:board", ix, ly, iw, lh);
            }
            PopFade(g, st);
        }
    }
}
