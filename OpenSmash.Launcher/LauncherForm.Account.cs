// SPDX-License-Identifier: GPL-3.0-or-later
// (OpenSmash 0.6) The player's account: the accounts service shared with SSBU Arena (OpenSmash.Core.Account), signed in
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
        }

        async void AccountSignOut()
        {
            if (_account == null) return;
            _discordOpen = false;
            await _account.SignOutAsync();
            new OnlinePass(_account, _repo).Remove();
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
                    try { string f = Path.Combine(_repo, "config", "crash_reports.txt"); on = !(File.Exists(f) && File.ReadAllText(f).Trim().StartsWith("off", StringComparison.OrdinalIgnoreCase)); } catch { }
                    _ = CrashReport.SendIfAnyAsync(_account, _repo, _gameStartedUtc, on, Updater.InstalledCommit(_repo), s => Updater.Log(_repo, s));
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
            var me = _account?.Me;
            if (me == null)
            {
                string label = _accountBusy ? "FINISH IN THE BROWSER  ×" : "SIGN IN WITH DISCORD";
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
            var me = _account?.Me;
            if (!_discordOpen || me == null) return;
            float s = _s, x = 40 * s, y = HeaderH * s + 8 * s, w = 400 * s, h = 200 * s;
            using (var p = Theme.Para(x, y, w, h, 10 * s))
            {
                using (var b = new SolidBrush(A(Color.FromArgb(240, Theme.Bg)))) g.FillPath(b, p);
                using (var pen = new Pen(A(Theme.Line), Math.Max(1, s))) g.DrawPath(pen, p);
            }
            AddHit("discord:panel", x, y, w, h);
            float ix = x + 24 * s, iy = y + 20 * s;
            Theme.Label(g, me.Display, FB(20 * s, true), A(Theme.Text), ix, iy, 24 * s);
            Theme.Label(g, me.DiscordId != null ? "Signed in with Discord" : "Signed in", FB(13 * s), A(Theme.Muted), ix, iy + 30 * s, 18 * s);
            Theme.Label(g, "Online play uses this name. One account for OpenSmash and SSBU Arena.", FB(12 * s), A(Theme.Dim), ix, iy + 56 * s, 16 * s);
            float bw = 130 * s, bh = 34 * s, bx = x + w - 24 * s - bw, by = y + h - 18 * s - bh;
            Theme.ButtonAnimated(g, "SIGN OUT", bx, by, bw, bh, s, primary: false, Hov("discord:signout", _hover == "discord:signout"), true, 12 * s, _alpha, press: Press("discord:signout"));
            AddHit("discord:signout", bx, by, bw, bh);
            if (_account?.Service.AccountPage != null)
            {
                float aw = 150 * s;
                Theme.ButtonAnimated(g, "ACCOUNT PAGE", ix, by, aw, bh, s, primary: false, Hov("account:page", _hover == "account:page"), true, 12 * s, _alpha, press: Press("account:page"));
                AddHit("account:page", ix, by, aw, bh);
            }
        }
    }
}
