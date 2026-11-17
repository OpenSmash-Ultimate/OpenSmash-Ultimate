using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.Linq;
using System.Net.Http;
using System.Threading;
using System.Threading.Tasks;

namespace OpenSmash.Launcher
{
    // The header's Discord account (DiscordAccount.cs): "SIGN IN WITH DISCORD", or the player's avatar, name and
    // global position ("#12"); clicked, a card with their GSP per fighter, the choice to show their name on the
    // leaderboard, and Sign out. Nothing of it without build\discord.txt (a build without Discord).
    public partial class LauncherForm
    {
        DiscordToken? _discord;
        bool _discordBusy, _discordOpen, _discordShown, _discordHasApp;
        string _discordError = "";
        Image? _discordAvatar;
        int _discordRank;
        Dictionary<int, int> _discordFighterRanks = new();
        List<FighterGsp> _discordRecords = new();
        CancellationTokenSource? _discordCancel;

        void DiscordInit()
        {
            _discordHasApp = DiscordAccount.AppId(_repo) != "";
            _discordShown = DiscordAccount.ShowsName(_repo);
            if (_discordHasApp) _ = DiscordRefresh();
        }

        async Task DiscordRefresh()
        {
            try
            {
                _discord = await DiscordAccount.Current(_repo, CancellationToken.None);
                await DiscordLoadExtras();
                // (the name the lobbies show follows the Discord account's: sent again at each start while shown)
                if (_discord != null && _discordShown) await DiscordAccount.SetShown(_repo, _discord, true, CancellationToken.None);
            }
            catch (Exception e) when (e is HttpRequestException or TaskCanceledException or System.IO.IOException) { }
            Invalidate();
        }

        async Task DiscordLoadExtras()
        {
            if (_discord == null) return;
            try
            {
                string url = DiscordAccount.AvatarUrl(_discord);
                if (url != "")
                {
                    var bytes = await _httpClient.GetByteArrayAsync(url);
                    using var ms = new System.IO.MemoryStream(bytes);
                    _discordAvatar = Image.FromStream(ms);
                }
            }
            catch (Exception e) when (e is HttpRequestException or TaskCanceledException or ArgumentException) { }
            await DiscordLoadGsp();
        }

        async Task DiscordLoadGsp()
        {
            try { _discordRecords = await DiscordAccount.Records(_repo, CancellationToken.None); }
            catch (Exception e) when (e is HttpRequestException or TaskCanceledException) { }
            try { (_discordRank, _discordFighterRanks) = await DiscordAccount.Ranks(_repo, _discord, CancellationToken.None); }
            catch (Exception e) when (e is HttpRequestException or TaskCanceledException or System.Text.Json.JsonException) { }
            Invalidate();
        }

        async void DiscordSignIn()
        {
            if (_discordBusy) return;
            _discordBusy = true; _discordError = "";
            _discordCancel = new CancellationTokenSource();
            Invalidate();
            try
            {
                _discord = await DiscordAccount.SignIn(_repo, _discordCancel.Token);
                await DiscordLoadExtras();
            }
            catch (OperationCanceledException) { _discordError = "Sign-in cancelled."; }
            catch (Exception e) when (e is InvalidOperationException or HttpRequestException or System.Text.Json.JsonException or KeyNotFoundException)
            {
                _discordError = e.Message;
            }
            _discordBusy = false;
            Invalidate();
        }

        async void DiscordToggleShown()
        {
            _discordShown = !_discordShown;
            Invalidate();
            try
            {
                int took = await DiscordAccount.SetShown(_repo, _discord, _discordShown, CancellationToken.None);
                _discordError = took == 0 && DiscordAccount.NsaId(_repo) == "" ? "Play online once first: then your name can be shown." : "";
            }
            catch (Exception e) when (e is System.IO.IOException or UnauthorizedAccessException) { _discordError = e.Message; }
            Invalidate();
        }

        void DiscordSignOut()
        {
            if (_discordShown) _ = DiscordAccount.SetShown(_repo, _discord, false, CancellationToken.None);
            _discordShown = false;
            DiscordAccount.SignOut(_repo);
            _discord = null; _discordAvatar = null; _discordRank = 0; _discordFighterRanks = new(); _discordOpen = false;
            Invalidate();
        }

        bool ActivateDiscordHit(string id)
        {
            switch (id)
            {
                case "discord:signin": DiscordSignIn(); return true;
                case "discord:cancel": _discordCancel?.Cancel(); return true;
                case "discord:card": _discordOpen = !_discordOpen; if (_discordOpen) _ = DiscordLoadGsp(); return true;
                case "discord:shown": DiscordToggleShown(); return true;
                case "discord:signout": DiscordSignOut(); return true;
                case "discord:close": _discordOpen = false; return true;
                case "discord:panel": return true;
            }
            if (_discordOpen && !id.StartsWith("discord:")) _discordOpen = false;
            return false;
        }

        // In the header, right of the brand.
        void PaintDiscordHeader(Graphics g, float x, float cy)
        {
            if (!_discordHasApp) return;
            float s = _s;
            var f = FB(13 * s, true);
            float sp = 13 * s * 0.08f;
            if (_discord == null)
            {
                string label = _discordBusy ? "FINISH IN THE BROWSER  ×" : "SIGN IN WITH DISCORD";
                float w = Theme.TextWidth(g, label, f, sp) + 32 * s, h = 32 * s;
                string id = _discordBusy ? "discord:cancel" : "discord:signin";
                Theme.ButtonAnimated(g, label, x, cy - h / 2, w, h, s, primary: false, Hov(id, _hover == id), true, 12 * s, _alpha, press: Press(id));
                AddHit(id, x, cy - h / 2, w, h);
                if (_discordError != "")
                    Theme.Label(g, _discordError, FB(12 * s), A(Theme.Bad), x + w + 12 * s, cy - 8 * s, 16 * s);
                return;
            }
            float a = 30 * s, ax = x, ay = cy - a / 2;
            using (var clip = new GraphicsPath())
            {
                clip.AddEllipse(ax, ay, a, a);
                if (_discordAvatar != null)
                {
                    var st = g.Save();
                    g.SetClip(clip);
                    g.DrawImage(_discordAvatar, ax, ay, a, a);
                    g.Restore(st);
                }
                else using (var b = new SolidBrush(A(Color.FromArgb(255, 88, 101, 242)))) g.FillPath(b, clip);
            }
            float hv = Hov("discord:card", _hover == "discord:card");
            float tx = ax + a + 10 * s;
            float nw = Theme.Label(g, _discord.Shown.ToUpperInvariant(), f, A(Theme.Mix(Theme.Text, Theme.Accent, hv * 0.5f)), tx, cy - 9 * s, 18 * s, sp);
            float rw = 0;
            if (_discordRank > 0) rw = Theme.Label(g, "  #" + _discordRank, FM(13 * s, true), A(Theme.Accent), tx + nw, cy - 9 * s, 18 * s);
            AddHit("discord:card", ax, ay, a + 10 * s + nw + rw, a);
        }

        // The account card under the header (on top of the view).
        void PaintDiscordCard(Graphics g, int W, int H)
        {
            if (!_discordOpen || _discord == null) return;
            float s = _s, x = 40 * s, y = HeaderH * s + 8 * s, w = 400 * s;
            var rows = _discordRecords.Take(10).ToList();
            float rowH = 26 * s, h = 120 * s + Math.Max(1, rows.Count) * rowH + 104 * s;
            using (var p = Theme.Para(x, y, w, h, 10 * s))
            {
                using (var b = new SolidBrush(A(Color.FromArgb(240, Theme.Bg)))) g.FillPath(b, p);
                using (var pen = new Pen(A(Theme.Line), Math.Max(1, s))) g.DrawPath(pen, p);
            }
            AddHit("discord:panel", x, y, w, h);
            float ix = x + 24 * s, iy = y + 20 * s, a = 56 * s;
            using (var clip = new GraphicsPath())
            {
                clip.AddEllipse(ix, iy, a, a);
                if (_discordAvatar != null) { var st = g.Save(); g.SetClip(clip); g.DrawImage(_discordAvatar, ix, iy, a, a); g.Restore(st); }
                else using (var b = new SolidBrush(A(Color.FromArgb(255, 88, 101, 242)))) g.FillPath(b, clip);
            }
            Theme.Label(g, _discord.Shown, FB(20 * s, true), A(Theme.Text), ix + a + 14 * s, iy + 4 * s, 24 * s);
            string sub = _discordRank > 0 ? $"#{_discordRank} in the GSP leaderboard" : "Not ranked yet";
            Theme.Label(g, sub, FB(13 * s), A(_discordRank > 0 ? Theme.Accent : Theme.Muted), ix + a + 14 * s, iy + 32 * s, 18 * s);
            float ty = iy + a + 22 * s;
            Theme.Label(g, "GSP PER FIGHTER", FM(12 * s, true), A(Theme.Dim), ix, ty, 16 * s, 12 * s * 0.12f);
            ty += 24 * s;
            if (rows.Count == 0)
                Theme.Label(g, "No online matches yet: play Quickplay to get a GSP.", FB(13 * s), A(Theme.Muted), ix, ty, rowH);
            foreach (var r in rows)
            {
                Theme.Label(g, DiscordAccount.FighterName(r.Fighter), FB(14 * s), A(Theme.Text), ix, ty, rowH);
                string gs = r.Gsp.ToString("N0", CultureInfo.InvariantCulture);
                if (_discordFighterRanks.TryGetValue(r.Fighter, out int fr) && fr > 0) gs = $"#{fr}   " + gs;
                Theme.Label(g, gs, FM(14 * s, true), A(Theme.Muted), ix, ty, rowH, 0, StringAlignment.Far, w - 48 * s);
                ty += rowH;
            }
            ty = Math.Max(ty, iy + a + 46 * s + rowH) + 12 * s;
            string shown = _discordShown ? "YOUR NAME IS ON THE LEADERBOARD: HIDE IT" : "SHOW MY NAME ON THE LEADERBOARD";
            float tw = w - 48 * s, th = 34 * s;
            Theme.ButtonAnimated(g, shown, ix, ty, tw, th, s, primary: !_discordShown, Hov("discord:shown", _hover == "discord:shown"), true, 12 * s, _alpha, press: Press("discord:shown"));
            AddHit("discord:shown", ix, ty, tw, th);
            if (_discordError != "") Theme.Label(g, _discordError, FB(12 * s), A(Theme.Bad), ix, ty + th + 4 * s, 16 * s);
            float bw = 130 * s, bh = 34 * s, bx = x + w - 24 * s - bw, by = y + h - 18 * s - bh;
            Theme.ButtonAnimated(g, "SIGN OUT", bx, by, bw, bh, s, primary: false, Hov("discord:signout", _hover == "discord:signout"), true, 12 * s, _alpha, press: Press("discord:signout"));
            AddHit("discord:signout", bx, by, bw, bh);
        }
    }
}
