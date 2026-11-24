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
    // (mastery, OpenSmash.Core/Mastery.cs) The GSP becomes mastery points: the account's total in place of its GSP and
    // place, and a "SSBU Arena · Competitive" card (Arena's launcher when installed, else its download page) with the
    // top mastery in place of the GSP leaderboard. OPENSMASH_MASTERY_UI=0: the GSP views as before.
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
        // the leaderboard rows' Discord profile pictures by URL: null while asked or when it could not be had (initials)
        readonly Dictionary<string, Image?> _boardAvatars = new();
        // (mastery) the home's "SSBU Arena · Competitive" card in place of the GSP leaderboard: Arena's launcher when it is
        // installed (OpenSmash.Core.ArenaEdition, looked for at each refresh), and the official lobbies' top mastery
        // (/mastery.json, summed over the regions; null with _topMasteryAsked: no lobby has it yet)
        List<OpenSmash.Core.MasteryRow>? _topMastery;
        bool _topMasteryAsked;
        string? _arenaExe;

        void CommunityInit()
        {
            RegionView();
            _ = RegionMeasure();   // (in the background: the launcher does not wait for it)
            _ = CommunityRefresh();
            _communityTimer = new System.Windows.Forms.Timer { Interval = (int)CommunityFeed.Every.TotalMilliseconds };
            _communityTimer.Tick += (_, _) => _ = CommunityRefresh();
            _communityTimer.Start();
            PresenceStart();
        }

        // The server region (OpenSmash.Core.ServerRegion). With Auto, every region's lobby is asked at each launcher start
        // and the closest is kept in config\server_region_auto.txt, which the game starts from; the players online and
        // the leaderboard are then that region's.
        async Task RegionMeasure()
        {
            if (OpenSmash.Core.OfficialLobby.Override(_repo) != null || OpenSmash.Core.ServerRegion.Setting(_repo) != OpenSmash.Core.ServerRegion.Auto) return;
            string before = OpenSmash.Core.OfficialLobby.Current(_repo);
            try { await OpenSmash.Core.ServerRegion.Measure(_repo); }
            catch (Exception e) when (e is System.IO.IOException or UnauthorizedAccessException or InvalidOperationException) { return; }
            RegionView();
            if (OpenSmash.Core.OfficialLobby.Current(_repo) != before) await CommunityAgain();
            Invalidate();
        }

        // What the pages show of the region, read when it changes (not at every paint): Auto's pick and its ping, an
        // install's own official server, the region the home page's numbers are of.
        string? _regionShown;
        string _regionLabel = "";
        bool _regionOverride;

        void RegionView()
        {
            _regionOverride = OpenSmash.Core.OfficialLobby.Override(_repo) != null;
            _regionShown = SettingDefaults.RegionShown(_repo, OpenSmash.Core.ServerRegion.Auto);
            _regionLabel = OpenSmash.Core.ServerRegion.Current(_repo)?.Label.ToUpperInvariant() ?? "";
        }

        // The region was changed in the settings: measured when it is Auto, and the home page asks the new lobby.
        async Task RegionChanged()
        {
            await RegionMeasure();
            RegionView();
            await CommunityAgain();
        }

        async Task CommunityAgain()
        {
            for (int i = 0; i < 100 && Volatile.Read(ref _communityBusy) == 1; i++) await Task.Delay(100);   // (an answer on its way is the old lobby's)
            _lobby = null;
            _lobbyAsked = false;
            await CommunityRefresh();
        }

        // The selected (or Auto's) region's players online from the accounts service (OpenSmash.Core.OnlineCount), or
        // null: no accounts service in this build, an official server of the install's own (no region), no fresh row
        // for the region, or the service not reached. Then the lobby's own status page is what is shown, as before.
        async Task<(int Players, int Rooms)?> RegionCount()
        {
            try
            {
                var account = _account;
                if (account == null || !account.Service.Configured || !OpenSmash.Core.OnlineCount.Wanted) return null;
                if (OpenSmash.Core.OfficialLobby.Override(_repo) != null) return null;
                string? region = OpenSmash.Core.ServerRegion.Current(_repo)?.Id;
                if (string.IsNullOrEmpty(region)) return null;
                var asked = account.AnonAsync(OpenSmash.Core.OnlineCount.Path(region));
                if (await Task.WhenAny(asked, Task.Delay(TimeSpan.FromSeconds(8))) != asked)
                {
                    _ = asked.ContinueWith(t => _ = t.Exception, TaskContinuationOptions.OnlyOnFaulted);   // (a late failure is nobody's)
                    return null;
                }
                return OpenSmash.Core.OnlineCount.Parse(await asked, DateTimeOffset.UtcNow);
            }
            catch (Exception e) when (e is OpenSmash.Core.AccountException or System.Net.Http.HttpRequestException or TaskCanceledException or InvalidOperationException or System.IO.IOException) { return null; }
        }

        async Task CommunityRefresh()
        {
            if (Interlocked.Exchange(ref _communityBusy, 1) == 1) return;
            try
            {
                var counted = RegionCount();   // (asked together with the lobby's own pages)
                bool mastery = OpenSmash.Core.Mastery.UiOn;
                // (the top mastery: the accounts service's board, the lobbies' lists only when it fails)
                var top = mastery && OpenSmash.Core.Mastery.TopOn
                    ? OpenSmash.Core.Mastery.TopAsync(_account, _httpClient, OpenSmash.Core.OfficialLobby.All(_repo), CancellationToken.None, 5) : null;
                var (status, board) = await CommunityFeed.Fetch(_httpClient, CancellationToken.None, _repo);
                var count = await counted;
                if (top != null) { _topMastery = await top ?? _topMastery; _topMasteryAsked = true; }
                _lobbyAsked = true;
                _lobbyDown = status == null && board == null && count == null;
                if (status != null) _lobby = status;
                // (the region's row of the accounts service's lobby_status, when it has one that is fresh: its players
                // are the count shown; the open rooms stay the lobby's own when it answered. No row: as before.)
                if (count is { } c)
                    _lobby = new LobbyStatus
                    {
                        Online = c.Players, OpenRooms = status?.OpenRooms ?? c.Rooms, Rooms = status?.Rooms ?? c.Rooms,
                        Arenas = status?.Arenas ?? 0, Playing = status?.Playing ?? 0, Ranked = status?.Ranked ?? 0,
                    };
                if (board != null && CommunityFeed.ParseBoard(board) is { } parsed)
                {
                    _top = parsed.Top;
                    _ranked = parsed.Ranked;
                    string nsa = await Task.Run(() => DiscordAccount.NsaId(_repo));
                    // (signed in: the account's row, as /gsp/me named it; else the install's)
                    _mine = CommunityFeed.Mine(board, _boardMeId ?? (nsa == "" ? "" : DiscordAccount.PlayerId(nsa)));
                    _ = BoardAvatarsLoad(_top);
                }
            }
            catch (Exception e) when (e is System.IO.IOException or UnauthorizedAccessException) { }
            finally { Interlocked.Exchange(ref _communityBusy, 0); }
            if (_view == "home") Invalidate();
            if (_view == "competitive") _ = RankedRefresh();   // (the COMPETITIVE page's board, asked again every minute while shown)
        }

        // The top rows' profile pictures, each URL asked once (a failed one stays initials until the launcher restarts).
        async Task BoardAvatarsLoad(List<BoardRow> rows)
        {
            foreach (var r in rows)
            {
                if (r.Avatar == "" || _boardAvatars.ContainsKey(r.Avatar)) continue;
                _boardAvatars[r.Avatar] = null;
                try
                {
                    var bytes = await _httpClient.GetByteArrayAsync(r.Avatar);
                    using var ms = new System.IO.MemoryStream(bytes);
                    using var img = Image.FromStream(ms);
                    _boardAvatars[r.Avatar] = new Bitmap(img);
                }
                catch (Exception e) when (e is System.Net.Http.HttpRequestException or TaskCanceledException or ArgumentException
                                          or System.IO.IOException or OutOfMemoryException) { }
            }
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
                case "community:me":
                    _discordOpen = !_discordOpen;
                    if (_discordOpen) _ = AccountMode ? AccountLoadGsp() : DiscordLoadGsp();
                    Invalidate(); return true;
                case "community:arena":   // (mastery) SSBU Arena's launcher, or its download page
                    OpenSmash.Core.Updater.Log(_repo, OpenSmash.Core.ArenaEdition.Open(url => OpenUrl(url)));
                    return true;
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

            bool mastery = OpenSmash.Core.Mastery.UiOn;
            bool accountIn = AccountMode && _account?.SignedIn == true;
            // the account
            if (_discordHasApp || _mine != null || _boardMeId != null || (mastery && accountIn))
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
                    string? accountName = AccountMode && _account?.SignedIn == true ? _account.Me?.Username : null;
                    using (var clip = new GraphicsPath())
                    {
                        clip.AddEllipse(ix, ay, a, a);
                        if (_discordAvatar != null) { var gs = g.Save(); g.SetClip(clip); g.DrawImage(_discordAvatar, ix, ay, a, a); g.Restore(gs); }
                        else
                        {
                            using (var b = new SolidBrush(A(_discord != null || accountName != null ? Color.FromArgb(255, 88, 101, 242) : Theme.Bg3))) g.FillPath(b, clip);
                            // (the signed-in account without a picture: its initial, as the header shows it)
                            if (_discord == null && !string.IsNullOrEmpty(accountName))
                                Theme.Label(g, accountName.Substring(0, 1).ToUpperInvariant(), FB(19 * s, true), A(Color.White), ix, ay + 10 * s, 24 * s, 0, StringAlignment.Center, a);
                        }
                    }
                    float tx = ix + a + 12 * s, tw = iw - a - 12 * s;
                    string who = _discord?.Shown ?? accountName ?? "You";
                    Theme.Label(g, who, FB(17 * s, true), A(Theme.Text), tx, ay - 1 * s, 24 * s, 0, StringAlignment.Near, tw);
                    int rank = _mine?.Rank > 0 ? _mine.Rank : _discordRank;
                    uint best = _mine?.Gsp ?? (_discordRecords.Count > 0 ? _discordRecords[0].Gsp : 0u);
                    int fighter = _mine?.Fighter ?? (_discordRecords.Count > 0 ? _discordRecords[0].Fighter : -1);
                    string line = best > 0
                        ? UiLocale.Format("{0} GSP  ·  {1}", best.ToString("N0", CultureInfo.InvariantCulture), DiscordAccount.FighterName(fighter))
                        : UiLocale.Text("No GSP yet: play Quickplay online");
                    if (mastery && AccountMode)
                    {
                        // (mastery: the account's total and its first fighter; its place on the accounts service's board
                        // when it has one, in the corner as the GSP place was)
                        rank = _mastery?.Rank is { Pos: > 0 and <= 99999 } mr ? (int)mr.Pos : 0;
                        line = _mastery is { } m && m.Fighters.Count > 0
                            ? UiLocale.Format("{0} mastery points  ·  {1}", OpenSmash.Core.Mastery.N(m.Total), OpenSmash.Core.Mastery.FighterName(m.Fighters[0].Fighter))
                            : UiLocale.Text(_masteryAsked && _mastery == null ? "Mastery points: not available yet" : "No mastery points yet: play online");
                    }
                    Theme.Label(g, line, FB(13 * s), A(Theme.Muted), tx, ay + 24 * s, 18 * s, 0, StringAlignment.Near, tw - 60 * s);
                    if (rank > 0)
                        Theme.Label(g, "#" + rank.ToString(CultureInfo.InvariantCulture), FD(26 * s), A(Theme.Accent), x, y + 14 * s, 40 * s, 0, StringAlignment.Far, w - pad);
                    if (_discord != null || (mastery && accountIn)) AddHit("community:me", x, y, w, ph);
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
                // (which region's lobby these numbers are: players on another region are not counted)
                string where = _regionLabel;
                Theme.Label(g, where == "" ? UiLocale.Text(head) : UiLocale.Text(head) + "  ·  " + where, FM(11 * s, true), A(Theme.Dim), ix + 16 * s, y + 12 * s, 16 * s, 11 * s * 0.12f);
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

            // (mastery) the top mastery in place of the GSP leaderboard: each row with its main fighter's icon, and the site's
            // full list below. SSBU Arena (its ranked, its launcher) has its own page: COMPETITIVE, LauncherForm.Competitive.cs.
            if (mastery)
            {
                if (OpenSmash.Core.Mastery.TopOn)
                {
                    float rowH = 26 * s, head = 34 * s, foot = 36 * s, isz = 20 * s;
                    var rows = _topMastery ?? new List<OpenSmash.Core.MasteryRow>();
                    float ph = head + Math.Max(1, rows.Count) * rowH + foot;
                    Panel(y, ph);
                    Theme.Label(g, "TOP MASTERY", FM(11 * s, true), A(Theme.Dim), ix, y + 12 * s, 16 * s, 11 * s * 0.12f);
                    float cy = y + head;
                    if (rows.Count == 0)
                        Theme.Label(g, !_topMasteryAsked ? "Loading..." : "Not available now.", FB(13 * s), A(Theme.Muted), ix, cy, rowH);
                    for (int i = 0; i < rows.Count; i++)
                    {
                        var r = rows[i];
                        Theme.Label(g, (i + 1).ToString(CultureInfo.InvariantCulture), FM(13 * s, true), A(i < 3 ? Theme.Accent : Theme.Dim), ix, cy, rowH, 0, StringAlignment.Far, 18 * s);
                        float nx = ix + 26 * s;
                        if (r.MainFighter != "") { DrawFighter(g, r.MainFighter, nx, cy + (rowH - isz) / 2, isz); }
                        nx += isz + 8 * s;
                        Theme.Label(g, OpenSmash.Core.Mastery.Who(r), FB(13 * s), A(Theme.Text), nx, cy, rowH, 0, StringAlignment.Near, iw - (nx - ix) - 70 * s);
                        Theme.Label(g, OpenSmash.Core.Mastery.N(r.Total), FM(13 * s, true), A(Theme.Muted), ix, cy, rowH, 0, StringAlignment.Far, iw);
                        cy += rowH;
                    }
                    // the site's full mastery list
                    float ly = y + ph - foot + 4 * s, lh = 26 * s;
                    float hv = Hov("community:board", _hover == "community:board");
                    Theme.Label(g, "FULL LEADERBOARD  →", FM(12 * s, true), A(Theme.Mix(Theme.Muted, Theme.Accent, hv)), ix, ly, lh, 12 * s * 0.08f);
                    AddHit("community:board", ix, ly, iw, lh);
                }
            }
            // the leaderboard's top 10
            else
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
                    float av = rowH - 4 * s, ax = ix + 30 * s, ay = ry + 2 * s;
                    using (var clip = new GraphicsPath())
                    {
                        clip.AddEllipse(ax, ay, av, av);
                        if (r.Avatar != "" && _boardAvatars.TryGetValue(r.Avatar, out var pic) && pic != null)
                        {
                            var gs = g.Save(); g.SetClip(clip, CombineMode.Intersect); g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                            g.DrawImage(pic, ax, ay, av, av); g.Restore(gs);
                        }
                        else
                        {
                            using (var b = new SolidBrush(A(Theme.Bg3))) g.FillPath(b, clip);
                            Theme.Label(g, CommunityFeed.Initials(CommunityFeed.Who(r)), FM(8 * s, true), A(Theme.Muted), ax, ay, av, 0, StringAlignment.Center, av);
                        }
                    }
                    Theme.Label(g, CommunityFeed.Who(r), FB(13 * s, me), A(c), ax + av + 6 * s, ry, rowH, 0, StringAlignment.Near, iw - 110 * s - av - 4 * s);
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
