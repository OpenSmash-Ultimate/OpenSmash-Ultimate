using System.Net.Http;
using Avalonia.Threading;
using OpenSmash.Core;
using OpenSmash.Launcher;

namespace OpenSmash;

// The home page's right column and the Discord account (main's LauncherForm.Community.cs / LauncherForm.Discord.cs):
// sign-in through the browser (DiscordAccount.SignIn, config/discord_token.txt shared with the game), sign out, the
// leaderboard name choice sent to every lobby, the lobby's status and top 10 asked every minute (CommunityFeed), and
// the launcher's own Rich Presence "In the launcher" (DiscordPresence, Windows only: the SDK library is fetched there
// alone), stopped while the game runs. Painted by Visuals/CommunityArtwork.cs from Surface.Community.
public sealed partial class MainWindow
{
    DispatcherTimer? _communityTimer, _presenceTimer;
    DiscordPresence? _presence;
    CancellationTokenSource? _discordCancel;
    bool _communityOn, _communityBusy;
    string? _communityRoot;

    void CommunityStart()
    {
        if (_closed) return;
        _communityOn = true;
        var c = Surface.Community;
        c.Enabled = true;
        if (_communityTimer == null)
        {
            _communityTimer = new DispatcherTimer { Interval = CommunityFeed.Every };
            _communityTimer.Tick += (_, _) => _ = CommunityRefresh();
            _communityTimer.Start();
        }
        if (_project != null && _project.Root != _communityRoot)
        {
            _communityRoot = _project.Root;
            c.HasApp = DiscordAccount.AppId(_communityRoot) != "";
            c.Shown = DiscordAccount.ShowsName(_communityRoot);
            c.Account = null; c.Avatar = null; c.Rank = 0; c.Records = new(); c.FighterRanks = new(); c.Open = false; c.Error = "";
            if (c.HasApp) _ = DiscordRefresh(_communityRoot);
            PresenceStop();
        }
        _ = CommunityRefresh();
        PresenceStart();
    }

    void CommunityStop()
    {
        _communityOn = false;
        _communityTimer?.Stop(); _communityTimer = null;
        _discordCancel?.Cancel();
        PresenceStop();
    }

    async Task CommunityRefresh()
    {
        if (_communityBusy) return;
        _communityBusy = true;
        try
        {
            var (status, board) = await CommunityFeed.Fetch(Http, CancellationToken.None);
            var account = Surface.Community.Account;
            Surface.Community.Apply(status, board, account?.UserId is { Length: > 0 } id ? DiscordAccount.AccountHash(id) : "");
        }
        finally { _communityBusy = false; }
        Surface.InvalidateVisual();
    }

    async Task DiscordRefresh(string root)
    {
        try
        {
            var t = await DiscordAccount.Current(root, CancellationToken.None);
            if (root != _communityRoot) return;
            Surface.Community.Account = t;
            await DiscordLoadExtras(root);
            // (the name the lobbies show follows the Discord account's: sent again at each start while shown)
            if (t != null && Surface.Community.Shown) await DiscordAccount.SetShown(root, t, true, CancellationToken.None);
        }
        catch (Exception e) when (e is HttpRequestException or TaskCanceledException or IOException or UnauthorizedAccessException) { }
        Surface.InvalidateVisual();
    }

    async Task DiscordLoadExtras(string root)
    {
        var t = Surface.Community.Account;
        if (t == null) return;
        try { if (DiscordAccount.AvatarUrl(t) is { Length: > 0 } url) Surface.Community.Avatar = await Http.GetByteArrayAsync(url); }
        catch (Exception e) when (e is HttpRequestException or TaskCanceledException) { }
        await DiscordLoadGsp(root);
    }

    async Task DiscordLoadGsp(string root)
    {
        var c = Surface.Community;
        try { c.Records = await DiscordAccount.Records(root, CancellationToken.None); }
        catch (Exception e) when (e is HttpRequestException or TaskCanceledException) { }
        try { (c.Rank, c.FighterRanks) = await DiscordAccount.Ranks(root, c.Account, CancellationToken.None); }
        catch (Exception e) when (e is HttpRequestException or TaskCanceledException or System.Text.Json.JsonException) { }
        Surface.InvalidateVisual();
    }

    async Task CommunityAction(string id)
    {
        var c = Surface.Community;
        string? root = _communityRoot;
        switch (id)
        {
            case "community:board": Commands.OpenPath(CommunityFeed.SitePage); break;
            case "community:cancel": _discordCancel?.Cancel(); break;
            case "community:me":
                c.Open = !c.Open; c.Error = "";
                if (c.Open && root != null) await DiscordLoadGsp(root);
                break;
            case "community:signin" when root != null && !c.Busy:
                c.Busy = true; c.Error = "";
                _discordCancel = new CancellationTokenSource();
                Surface.InvalidateVisual();
                try
                {
                    c.Account = await DiscordAccount.SignIn(root, _discordCancel.Token);
                    await DiscordLoadExtras(root);
                    if (c.Shown) await DiscordAccount.SetShown(root, c.Account, true, CancellationToken.None);
                    _ = CommunityRefresh();   // (their own row in the top 10)
                }
                catch (OperationCanceledException) { c.Error = "Sign-in cancelled."; }
                catch (Exception e) when (e is InvalidOperationException or HttpRequestException or System.Text.Json.JsonException or KeyNotFoundException or IOException or System.ComponentModel.Win32Exception)
                {
                    c.Error = e.Message;
                }
                finally { c.Busy = false; _discordCancel.Dispose(); _discordCancel = null; }
                break;
            case "community:shown" when root != null:
                c.Shown = !c.Shown;
                Surface.InvalidateVisual();
                try
                {
                    int took = await DiscordAccount.SetShown(root, c.Account, c.Shown, CancellationToken.None);
                    c.Error = took == 0 && DiscordAccount.NsaId(root) == "" ? "Play online once first." : "";
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { c.Error = e.Message; }
                break;
            case "community:signout" when root != null:
                if (c.Shown) _ = DiscordAccount.SetShown(root, c.Account, false, CancellationToken.None);
                c.Shown = false;
                DiscordAccount.SignOut(root);
                c.Account = null; c.Avatar = null; c.Rank = 0; c.FighterRanks = new(); c.Records = new(); c.Open = false; c.Error = "";
                break;
        }
        Surface.InvalidateVisual();
    }

    // ---- Rich Presence: the launcher's, until the game starts -------------------------------------------------------
    void PresenceStart()
    {
        if (!_communityOn || _closed || _presence != null || _communityRoot == null) return;
        string app = DiscordAccount.AppId(_communityRoot);
        if (app == "") return;
        try { _presence = DiscordPresence.Start(Project.GameDirectory(_communityRoot), app); }
        catch (Exception e) when (e is DllNotFoundException or BadImageFormatException or System.Runtime.InteropServices.SEHException) { _presence = null; }
        if (_presence == null) return;
        _presence.Show("In the launcher");
        _presenceTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(100) };
        _presenceTimer.Tick += (_, _) =>
        {
            try { _presence?.Tick(); }
            catch (System.Runtime.InteropServices.SEHException) { PresenceStop(); }
        };
        _presenceTimer.Start();
    }

    void PresenceStop()
    {
        _presenceTimer?.Stop(); _presenceTimer = null;
        _presence?.Dispose(); _presence = null;
    }
}
