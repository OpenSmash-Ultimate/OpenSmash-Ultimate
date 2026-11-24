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
        // (the fighters' stock icons, downloaded once each: OpenSmash.Core.Fighters; the page is drawn again when one is there)
        c.IconBytes ??= key => Fighters.Icon(Http, key, () => Dispatcher.UIThread.Post(() => Surface.InvalidateVisual()));
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
            c.Account = null; c.Avatar = null; c.Rank = 0; c.Records = new(); c.FighterRanks = new(); c.Open = false; c.Error = "";
            AccountInit(_communityRoot);   // (v0.5.0: MainWindow.Account.cs; the account replaces the Discord token)
            c.Shown = AccountMode || DiscordAccount.ShowsName(_communityRoot);   // (a signed-in account is listed by default)
            bool discord = c.HasApp && !AccountMode, region = _backgroundActions;
            PresenceStop();
            // (macOS: one after the other. Started together, their HTTP, UDP and DNS calls on the thread pool's workers
            // crashed the launcher at start on .NET 8 most times (#542, found and tested by volcoice); elsewhere at once)
            if (OperatingSystem.IsMacOS())
            {
                _ = CommunityStartInTurn(_communityRoot, discord, region);
                PresenceStart();
                return;
            }
            if (discord) _ = DiscordRefresh(_communityRoot);
            if (region) _ = RegionMeasure(_communityRoot);
        }
        _ = CommunityRefresh();
        PresenceStart();
    }

    async Task CommunityStartInTurn(string root, bool discord, bool region)
    {
        if (discord) await DiscordRefresh(root);
        if (region) await RegionMeasure(root);
        await CommunityRefresh();
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
            // (mastery: the top mastery from the accounts service's board, the official lobbies' lists only when it fails;
            // and Arena's launcher when installed)
            var c0 = Surface.Community;
            var top = Mastery.UiOn && Mastery.TopOn && _project != null ? Mastery.TopAsync(_account, Http, OfficialLobby.All(_project.Root), CancellationToken.None, 5) : null;
            if (Mastery.UiOn && Mastery.ArenaCardOn) c0.ArenaExe = await Task.Run(() => ArenaEdition.Find());
            var (status, board) = await CommunityFeed.Fetch(Http, CancellationToken.None, _project?.Root);
            if (top != null) { c0.TopMastery = await top ?? c0.TopMastery; c0.TopMasteryAsked = true; }
            var account = Surface.Community.Account;
            // (v0.5.0: an account's row is the id the lobby gave with the ticket; a Discord token's is its account hash)
            string mine = AccountMode ? Surface.Community.BoardId ?? "" : account?.UserId is { Length: > 0 } id ? DiscordAccount.AccountHash(id) : "";
            Surface.Community.Apply(status, board, mine);
        }
        finally { _communityBusy = false; }
        Surface.InvalidateVisual();
        if (Surface.CurrentView == "competitive") await RankedRefresh();   // (asked again every minute while shown)
    }

    // The COMPETITIVE page's board (Visuals/CompetitiveArtwork.cs): SSBU Arena's ranked top 10 for the region picked there.
    bool _rankedBusy;
    DateTime _rankedAt = DateTime.MinValue;
    async Task RankedRefresh(bool force = false)
    {
        var c = Surface.Community;
        if (_rankedBusy || (!force && c.RankedAsked && DateTime.UtcNow - _rankedAt < TimeSpan.FromSeconds(55))) return;
        _rankedBusy = true;
        try
        {
            int region = c.RankedRegion;
            c.ArenaExe = await Task.Run(() => ArenaEdition.Find());
            var rows = await RankedBoard.TopAsync(_account, RankedBoard.Regions[Math.Clamp(region, 0, RankedBoard.Regions.Length - 1)], 10);
            if (region != c.RankedRegion) return;   // (another region was picked meanwhile: its own answer follows)
            c.RankedFailed = rows == null;
            if (rows != null) c.RankedRows = rows;
            c.RankedAsked = true;
            _rankedAt = DateTime.UtcNow;
        }
        finally { _rankedBusy = false; }
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
        if (AccountMode && root != null && id is "community:signin" or "community:shown" or "community:manage" or "community:signout")
        {
            await AccountAction(id, root);   // (MainWindow.Account.cs)
            return;
        }
        switch (id)
        {
            case "community:board": Commands.OpenPath(CommunityFeed.SitePage); break;
            case "community:competitive": await RankedRefresh(); break;   // (the COMPETITIVE page was opened)
            case "community:arena-board": Commands.OpenPath(RankedBoard.LeaderboardPage); break;
            case var region when region.StartsWith("community:region:") && int.TryParse(region["community:region:".Length..], out int r) && r >= 0 && r < RankedBoard.Regions.Length:
                if (r != c.RankedRegion) { c.RankedRegion = r; c.RankedRows = null; c.RankedAsked = false; Surface.InvalidateVisual(); await RankedRefresh(force: true); }
                break;
            case "community:arena":   // (mastery) SSBU Arena's launcher (Windows, installed), else its download page
                if (root != null) Updater.Log(root, ArenaEdition.Open(Commands.OpenPath));
                else ArenaEdition.Open(Commands.OpenPath);
                break;
            case "community:cancel": _discordCancel?.Cancel(); break;
            case "community:me":
                c.Open = !c.Open; c.Error = "";
                if (c.Open && root != null) await (AccountMode ? AccountLoadGsp(root) : DiscordLoadGsp(root));
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

    // (v0.5.0) The server region (OpenSmash.Core.ServerRegion, as the WinForms launcher's LauncherForm.Community.cs): with
    // Auto, every region's lobby is asked at each start and the closest is kept in config/server_region_auto.txt, which
    // the game starts from; the players online and the leaderboard are then that region's.
    async Task RegionMeasure(string root)
    {
        if (OfficialLobby.Override(root) != null || ServerRegion.Setting(root) != ServerRegion.Auto) return;
        try { await ServerRegion.Measure(root); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Net.Sockets.SocketException) { return; }
        if (root != _communityRoot) return;
        Surface.RegionMeasured();
        await CommunityRefresh();
    }

    /// <summary>the region was changed in the settings: measured when Auto, and the lobby's numbers asked again</summary>
    internal async Task RegionChanged()
    {
        if (_communityRoot == null || !_backgroundActions) return;
        await RegionMeasure(_communityRoot);
        await CommunityRefresh();
        if (AccountMode) await AccountLoadGsp(_communityRoot);
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
