// SPDX-License-Identifier: GPL-3.0-or-later
// (v0.5.0) The player's account: the accounts service shared with SSBU Arena (OpenSmash.Core.Account), signed in
// with Discord in the browser. It is the online identity: at Play the launcher writes a lobby ticket for the game
// (build\cache\online_pass.txt, SSBU_ONLINE_TICKET_FILE) and renews it while the game runs; the official lobby lets in
// only signed-in players and lists them by their username. After a game, a crash/freeze/rollback report it wrote is sent
// (signed in only; config\crash_reports.txt "off" stops it). The header shows the account where #368 showed the Discord
// token (LauncherForm.Discord.cs); without a configured service (a local build) the old Discord sign-in stays.
using OpenSmash.Core;

namespace OpenSmash.Launcher
{
    partial class LauncherForm
    {
        Account? _account;
        OnlinePass? _pass;
        bool _accountBusy;
        string _accountError = "";
        CancellationTokenSource? _accountCancel;
        DateTime _gameStartedUtc;
        string? _boardMeId;   // (v0.5.0) the account's row on the official lobby's leaderboard, from /gsp/me (null: not asked yet)
        bool _accountShown = true;   // (v0.5.0) its name shown on the leaderboard (/gsp/me "show"; listed by default)
        bool _accountShownBusy;
        // (mastery) the account's mastery points, summed over the official lobbies (OpenSmash.Core.Mastery); null with
        // _masteryAsked: no lobby has them yet ("not available"). OPENSMASH_MASTERY_UI=0: the GSP views as before.
        MasteryMe? _mastery;
        bool _masteryAsked;

        /// <summary>the account service is set up in this build: the account replaces #368's Discord token</summary>
        bool AccountMode => _account != null && _account.Service.Configured;

        void AccountInit()
        {
            try { _account = new Account(_repo) { OpenBrowser = url => OpenUrl(url) }; }
            catch (Exception e) { Updater.Log(_repo, "accounts: " + e.Message); _account = null; }
            if (AccountMode) _ = AccountResume();
        }

        static void OpenUrl(string url)
        {
            try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(url) { UseShellExecute = true }); } catch { }
        }

        async Task AccountResume()
        {
            if (_account == null) return;
            string err = "";
            await _account.ResumeAsync(e => err = e);
            if (err != "") _accountError = "Offline: " + err;
            Invalidate();
            await AccountLoadGsp();
        }

        /// <summary>(v0.5.0) the account's GSP and places, from the official lobbies with the lobby ticket as the proof
        /// (OpenSmash.Core.LobbyBoard): the game's records are the account's there, not the install's</summary>
        async Task AccountLoadGsp()
        {
            if (!AccountMode || _account == null || !_account.SignedIn) return;
            string? ticket;
            try { ticket = await _account.LobbyTicketAsync(); }
            catch (Exception e) { Updater.Log(_repo, "leaderboard: no lobby ticket now: " + e.Message); ticket = LobbyBoard.StoredTicket(_repo); }
            var lobbies = OfficialLobby.All(_repo);
            // (mastery: the accounts service, the source; the lobbies with the ticket only when it fails. Asked together.)
            var mastery = Mastery.UiOn ? Mastery.MineAsync(_account, _httpClient, lobbies, ticket ?? "", CancellationToken.None) : null;
            var me = string.IsNullOrEmpty(ticket) ? null : await LobbyBoard.MeAsync(_httpClient, lobbies, ticket, CancellationToken.None);
            if (mastery != null)
            {
                _mastery = await mastery ?? _mastery;   // (a lobby down for a while: the last answer kept)
                _masteryAsked = true;
                Invalidate();
            }
            if (me == null) return;
            _discordRecords = me.Fighters.Where(f => f.Gsp > 0).Select(f => new FighterGsp { Fighter = f.Fighter, Gsp = f.Gsp, Rank = f.Rank }).ToList();
            _discordRank = me.Rank;
            _discordFighterRanks = me.Fighters.Where(f => f.Rank > 0).ToDictionary(f => f.Fighter, f => f.Rank);
            _boardMeId = me.Id == "" ? null : me.Id;
            _accountShown = me.Shown;
            Invalidate();
        }

        /// <summary>(v0.5.0) the account's name shown on, or hidden from, the official lobbies' leaderboard, with the ticket
        /// as the proof (OpenSmash.Core.LobbyBoard.SetShownAsync); put back when no lobby took it</summary>
        async void AccountToggleShown()
        {
            if (!AccountMode || _account == null || !_account.SignedIn || _accountShownBusy) return;
            _accountShownBusy = true; _accountShown = !_accountShown; _accountError = "";
            Invalidate();
            string? ticket;
            try { ticket = await _account.LobbyTicketAsync(); }
            catch (AccountException) { ticket = LobbyBoard.StoredTicket(_repo); }
            int took = string.IsNullOrEmpty(ticket) ? 0 : await LobbyBoard.SetShownAsync(_httpClient, OfficialLobby.All(_repo), ticket, _accountShown, CancellationToken.None);
            if (took == 0) { _accountShown = !_accountShown; _accountError = "The lobby did not answer: try again."; }
            _accountShownBusy = false;
            Invalidate();
        }

        async void AccountSignIn()
        {
            if (_account == null || _accountBusy) return;
            _accountBusy = true; _accountError = "";
            _accountCancel = new CancellationTokenSource();
            Invalidate();
            try { await _account.SignInWithBrowserAsync(_accountCancel.Token); }
            catch (OperationCanceledException) { _accountError = "Sign-in cancelled."; }
            catch (AccountException e) { _accountError = e.Message; }
            catch (Exception e) { _accountError = "The sign-in did not work: " + e.Message; Updater.Log(_repo, "sign-in: " + e); }
            _accountBusy = false;
            Invalidate();
            await AccountLoadGsp();
        }

        async void AccountSignOut()
        {
            if (_account == null) return;
            _discordOpen = false;
            await _account.SignOutAsync();
            new OnlinePass(_account, _repo).Remove();
            _boardMeId = null; _discordRecords = new(); _discordRank = 0; _discordFighterRanks = new(); _accountShown = true;
            _mastery = null; _masteryAsked = false;
            Invalidate();
        }

        /// <summary>before the game starts: a fresh ticket in the file the game reads (null: no file, not signed in)</summary>
        async Task<string?> AccountPassForGame()
        {
            if (!AccountMode || _account == null) return null;
            _pass?.Dispose();
            _pass = new OnlinePass(_account, _repo) { Log = s => Updater.Log(_repo, s) };
            if (!_account.SignedIn) { _pass.Remove(); return null; }
            return await _pass.WriteAsync() ? _pass.File : null;
        }

        /// <summary>the game started (proc): the ticket kept fresh while it runs; at its end, a report it wrote is sent</summary>
        void AccountWatchGame(System.Diagnostics.Process? proc)
        {
            _gameStartedUtc = DateTime.UtcNow;
            if (proc == null) return;
            if (_pass != null && File.Exists(_pass.File)) _pass.KeepFresh();
            try
            {
                proc.EnableRaisingEvents = true;
                proc.Exited += (_, _) =>
                {
                    _pass?.Dispose();
                    if (_account == null) return;
                    bool on = true;
                    on = !FileSaysOff(Path.Combine(_repo, "config", "crash_reports.txt"));
                    _ = CrashReport.SendIfAnyAsync(_account, _repo, _gameStartedUtc, on, Updater.InstalledCommit(_repo), s => Updater.Log(_repo, s));
                    // (the session's performance summary: aggregates only, signed in only; config\telemetry.txt "off" stops it)
                    _ = PerfReport.SendIfAnyAsync(_account, _repo, _gameStartedUtc, Updater.InstalledCommit(_repo), s => Updater.Log(_repo, s));
                    try { BeginInvoke(new Action(() => _ = AccountLoadGsp())); } catch (InvalidOperationException) { }   // (the GSP the game just earned)
                };
            }
            catch { }
        }

        bool ActivateAccountHit(string id)
        {
            if (!AccountMode) return false;
            switch (id)
            {
                case "discord:signin": AccountSignIn(); return true;
                case "discord:cancel": _accountCancel?.Cancel(); return true;
                case "discord:card": _discordOpen = !_discordOpen; return true;
                case "discord:signout": AccountSignOut(); return true;
                case "discord:close": _discordOpen = false; return true;
                case "discord:panel": return true;
                case "account:page": if (_account?.Service.AccountPage != null) OpenUrl(_account.Service.AccountPage); return true;
                case "account:shown": AccountToggleShown(); return true;
            }
            if (_discordOpen && !id.StartsWith("discord:")) _discordOpen = false;
            return false;
        }

        // In the header, right of the brand (as #368's Discord sign-in was)
        void PaintAccountHeader(Graphics g, float x, float cy)
        {
            float s = _s;
            var f = FB(13 * s, true);
            float sp = 13 * s * 0.08f;
            var me = _account != null && _account.SignedIn ? _account.Me : null;   // (signed in = the profile is read)
            if (me == null)
            {
                // (the accounts site offers Discord and e-mail; without it the browser goes straight to Discord)
                string label = _accountBusy ? "FINISH IN THE BROWSER  ×" : _account?.Service.SignInPage != null ? "SIGN IN" : "SIGN IN WITH DISCORD";
                float w = Theme.TextWidth(g, label, f, sp) + 32 * s, h = 32 * s;
                string id = _accountBusy ? "discord:cancel" : "discord:signin";
                Theme.ButtonAnimated(g, label, x, cy - h / 2, w, h, s, primary: false, Hov(id, _hover == id), true, 12 * s, _alpha, press: Press(id));
                AddHit(id, x, cy - h / 2, w, h);
                if (_accountError != "")
                    Theme.Label(g, _accountError, FB(12 * s), A(Theme.Bad), x + w + 12 * s, cy - 8 * s, 16 * s);
                return;
            }
            float a = 30 * s, ax = x, ay = cy - a / 2;
            using (var b = new SolidBrush(A(Color.FromArgb(255, 88, 101, 242)))) g.FillEllipse(b, ax, ay, a, a);
            Theme.Label(g, me.Username.Substring(0, 1).ToUpperInvariant(), FB(14 * s, true), A(Color.White), ax, ay + 6 * s, 18 * s, 0, StringAlignment.Center, a);
            float hv = Hov("discord:card", _hover == "discord:card");
            float nw = Theme.Label(g, me.Username.ToUpperInvariant(), f, A(Theme.Mix(Theme.Text, Theme.Accent, hv * 0.5f)), ax + a + 10 * s, cy - 9 * s, 18 * s, sp);
            AddHit("discord:card", ax, ay, a + 10 * s + nw, a);
        }

        // The account card under the header
        void PaintAccountCard(Graphics g, int W, int H)
        {
            var me = _account != null && _account.SignedIn ? _account.Me : null;   // (signed in = the profile is read)
            if (!_discordOpen || me == null) return;
            float s = _s, x = 40 * s, y = HeaderH * s + 8 * s, w = 400 * s, h = 244 * s;
            bool mastery = Mastery.UiOn;
            var mrows = mastery && _mastery != null ? _mastery.Fighters.Take(8).ToList() : new List<MasteryFighter>();
            float mRowH = 22 * s, mH = mastery ? 26 * s + 18 * s + Math.Max(1, mrows.Count) * mRowH + 22 * s + 8 * s : 0;
            h += mH;
            // the note under the name (or a show/hide no lobby took) wraps inside the card; the card grows for a second line
            string note = _accountError != "" ? _accountError : "Online play uses this name. One account for OpenSmash and SSBU Arena.";
            var noteFont = FB(12 * s);
            float noteW = w - 48 * s;
            float noteH = (float)Math.Ceiling(g.MeasureString(UiLocale.Text(note), noteFont, (int)Math.Max(1, noteW)).Height);
            float noteExtra = Math.Max(0, noteH - 20 * s);
            h += noteExtra;
            using (var p = Theme.Para(x, y, w, h, 10 * s))
            {
                using (var b = new SolidBrush(A(Color.FromArgb(240, Theme.Bg)))) g.FillPath(b, p);
                using (var pen = new Pen(A(Theme.Line), Math.Max(1, s))) g.DrawPath(pen, p);
            }
            AddHit("discord:panel", x, y, w, h);
            float ix = x + 24 * s, iy = y + 20 * s;
            Theme.Label(g, me.Display, FB(20 * s, true), A(Theme.Text), ix, iy, 24 * s);
            Theme.Label(g, me.DiscordId != null ? "Signed in with Discord" : "Signed in", FB(13 * s), A(Theme.Muted), ix, iy + 30 * s, 18 * s);
            Theme.DrawWrapped(g, note, noteFont, A(_accountError != "" ? Theme.Bad : Theme.Dim), ix, iy + 54 * s, noteW, noteH + 2 * s);
            if (mastery) PaintAccountMastery(g, ix, iy + 84 * s + noteExtra, w - 48 * s, mrows, mRowH);
            float bw = 130 * s, bh = 34 * s, bx = x + w - 24 * s - bw, by = y + h - 18 * s - bh;
            // (v0.5.0) the leaderboard choice: the official lobbies list a signed-in account by its name unless it hides
            string shownLabel = _accountShown ? "HIDE MY NAME ON THE LEADERBOARD" : "SHOW MY NAME ON THE LEADERBOARD";
            float sw = w - 48 * s, sy = by - bh - 10 * s;
            Theme.ButtonAnimated(g, shownLabel, ix, sy, sw, bh, s, primary: !_accountShown, Hov("account:shown", _hover == "account:shown"), true, 12 * s, _alpha, press: Press("account:shown"));
            AddHit("account:shown", ix, sy, sw, bh);
            Theme.ButtonAnimated(g, "SIGN OUT", bx, by, bw, bh, s, primary: false, Hov("discord:signout", _hover == "discord:signout"), true, 12 * s, _alpha, press: Press("discord:signout"));
            AddHit("discord:signout", bx, by, bw, bh);
            if (_account?.Service.AccountPage != null)
            {
                float aw = 150 * s;
                Theme.ButtonAnimated(g, "ACCOUNT PAGE", ix, by, aw, bh, s, primary: false, Hov("account:page", _hover == "account:page"), true, 12 * s, _alpha, press: Press("account:page"));
                AddHit("account:page", ix, by, aw, bh);
            }
        }

        /// <summary>(mastery) the card's MASTERY section: the account's total, then per fighter the points (summed over the
        /// regions, with the largest veteran bonus), games and wins. Points only: no levels, no tiers.</summary>
        void PaintAccountMastery(Graphics g, float ix, float y, float iw, List<MasteryFighter> rows, float rowH)
        {
            float s = _s;
            Theme.Label(g, "MASTERY", FM(12 * s, true), A(Theme.Dim), ix, y, 16 * s, 12 * s * 0.12f);
            if (_mastery != null)
            {
                // (the place among all the accounts, from the accounts service, when it has one and it fits)
                string total = UiLocale.Format("{0} POINTS IN ALL", Mastery.N(_mastery.Total));
                string withPlace = _mastery.Rank != null ? Mastery.Place(_mastery.Rank) + "  ·  " + total : total;
                var hf = FM(12 * s, true);
                if (Theme.TextWidth(g, withPlace, hf, 0, false) > iw - Theme.TextWidth(g, "MASTERY", hf, 12 * s * 0.12f) - 16 * s) withPlace = total;
                Theme.Label(g, withPlace, hf, A(Theme.Accent), ix, y, 16 * s, 0, StringAlignment.Far, iw);
            }
            y += 26 * s;
            float cWins = iw, cGames = iw - 60 * s, cPoints = iw - 124 * s;
            var head = FM(10 * s, true);
            Theme.Label(g, "POINTS", head, A(Theme.Dim), ix, y, 14 * s, 0, StringAlignment.Far, cPoints);
            Theme.Label(g, "GAMES", head, A(Theme.Dim), ix, y, 14 * s, 0, StringAlignment.Far, cGames);
            Theme.Label(g, "WINS", head, A(Theme.Dim), ix, y, 14 * s, 0, StringAlignment.Far, cWins);
            y += 18 * s;
            if (rows.Count == 0)
            {
                string none = !_masteryAsked ? "Loading..." : _mastery == null ? "Mastery points are not available on the lobby yet." : "No mastery points yet: play Quickplay or Battle Arena online.";
                Theme.Label(g, none, FB(13 * s), A(Theme.Muted), ix, y, rowH);
                y += rowH;
            }
            foreach (var r in rows)
            {
                float isz = 18 * s;
                DrawFighter(g, r.Fighter, ix, y + (rowH - isz) / 2, isz);   // (its stock icon: LauncherForm.Competitive.cs)
                Theme.Label(g, Mastery.FighterName(r.Fighter), FB(14 * s), A(Theme.Text), ix + isz + 8 * s, y, rowH);
                Theme.Label(g, Mastery.N(r.Total), FM(14 * s, true), A(Theme.Text), ix, y, rowH, 0, StringAlignment.Far, cPoints);
                Theme.Label(g, Mastery.N(r.Games), FM(13 * s), A(Theme.Muted), ix, y, rowH, 0, StringAlignment.Far, cGames);
                Theme.Label(g, Mastery.N(r.Wins), FM(13 * s), A(Theme.Muted), ix, y, rowH, 0, StringAlignment.Far, cWins);
                y += rowH;
            }
            Theme.Label(g, "Points for playing, they only go up. Ranked lives in SSBU Arena.", FB(11 * s), A(Theme.Dim), ix, y + 4 * s, 16 * s);
        }
    }
}
