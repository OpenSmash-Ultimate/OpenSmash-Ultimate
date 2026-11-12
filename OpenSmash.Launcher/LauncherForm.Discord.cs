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
    // position ("#12"); clicked, a card with their GSP per fighter and Sign out.
    public partial class LauncherForm
    {
        DiscordToken? _discord;
        bool _discordBusy, _discordOpen;
        string _discordError = "";
        Image? _discordAvatar;
        int _discordRank;
        Dictionary<int, int> _discordFighterRanks = new();
        List<(int Fighter, uint Gsp)> _discordRecords = new();
        CancellationTokenSource? _discordCancel;

        void DiscordInit()
        {
            _discordRecords = DiscordAccount.Records(_repo);
            _ = DiscordRefresh();
        }

        async Task DiscordRefresh()
        {
            try
            {
                _discord = await DiscordAccount.Current(_repo, CancellationToken.None);
                await DiscordLoadExtras();
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
            try { (_discordRank, _discordFighterRanks) = await DiscordAccount.Ranks(_discord.UserId, CancellationToken.None); }
            catch (Exception e) when (e is HttpRequestException or TaskCanceledException or System.Text.Json.JsonException) { }
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

        void DiscordSignOut()
        {
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
                case "discord:card": _discordOpen = !_discordOpen; if (_discordOpen) _discordRecords = DiscordAccount.Records(_repo); return true;
                case "discord:signout": DiscordSignOut(); return true;
                case "discord:close": _discordOpen = false; return true;
            }
            if (_discordOpen && !id.StartsWith("discord:")) _discordOpen = false;
            return false;
        }

        // In the header, right of the brand.
        void PaintDiscordHeader(Graphics g, float x, float cy)
        {
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
            float s = _s, x = 40 * s, y = HeaderH * s + 8 * s, w = 380 * s;
            var rows = _discordRecords.Take(10).ToList();
            float rowH = 26 * s, h = 120 * s + Math.Max(1, rows.Count) * rowH + 56 * s;
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
            foreach (var (fighter, gsp) in rows)
            {
                Theme.Label(g, DiscordAccount.FighterName(fighter), FB(14 * s), A(Theme.Text), ix, ty, rowH);
                string gs = gsp.ToString("N0", CultureInfo.InvariantCulture);
                if (_discordFighterRanks.TryGetValue(fighter, out int fr) && fr > 0) gs = $"#{fr}   " + gs;
                Theme.Label(g, gs, FM(14 * s, true), A(Theme.Muted), ix, ty, rowH, 0, StringAlignment.Far, w - 48 * s);
                ty += rowH;
            }
            float bw = 130 * s, bh = 34 * s, bx = x + w - 24 * s - bw, by = y + h - 18 * s - bh;
            Theme.ButtonAnimated(g, "SIGN OUT", bx, by, bw, bh, s, primary: false, Hov("discord:signout", _hover == "discord:signout"), true, 12 * s, _alpha, press: Press("discord:signout"));
            AddHit("discord:signout", bx, by, bw, bh);
        }
    }
}
