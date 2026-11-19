using System.Net.Http;
using OpenSmash.Core;
using OpenSmash.Launcher;

namespace OpenSmash;

// (v0.5.0) The player's account on Windows, Linux and macOS: the accounts service shared with SSBU Arena
// (OpenSmash.Core.Account, the same code as the WinForms launcher's LauncherForm.Account.cs). Sign-in in the player's
// browser: the accounts site, or Discord straight from Supabase with PKCE, back to a loopback address of this launcher.
// The session stays on this computer for this user only (Account.cs: DPAPI on Windows; elsewhere config/session.dat
// made 0600 before anything is written to it, readable by this user alone). At Play a lobby ticket goes to
// build/cache/online_pass.txt (OnlinePass: 0600 off Windows too) and SSBU_ONLINE_TICKET_FILE names it to the game, which
// reads it again while it runs; the launcher renews it at half its life until the game exits. The official lobbies let in
// only a game that sends one. The account's GSP, place and leaderboard choice are asked of the official lobbies with the
// ticket as the proof (OpenSmash.Core.LobbyBoard), as the WinForms launcher does. With no service in the build (a build
// without OpenSmash.Core/account_service.json values) the older Discord sign-in (MainWindow.Discord.cs) stays.
public sealed partial class MainWindow
{
    Account? _account;
    string? _accountRoot;
    OnlinePass? _pass;
    Task? _accountResume;

    bool AccountMode => _account != null && _account.Service.Configured;
    internal Account? AccountForTests => _account;

    /// <summary>the account of the project in root (made again when the project changes); resumed in the background
    /// only (headless tests never reach a real service)</summary>
    internal void AccountInit(string root)
    {
        if (_accountRoot == root && _account != null) return;
        _accountRoot = root;
        try { _account = new Account(root) { OpenBrowser = Commands.OpenPath }; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException or ArgumentException) { _account = null; }
        var c = Surface.Community;
        c.AccountMode = AccountMode;
        c.AccountPage = _account?.Service.AccountPage != null;
        c.Me = null; c.BoardId = null;
        if (AccountMode && _backgroundActions) _accountResume = AccountResume(root);
    }

    async Task AccountResume(string root)
    {
        if (_account == null) return;
        string err = "";
        await _account.ResumeAsync(e => err = e);
        if (root != _accountRoot) return;
        Surface.Community.Me = _account.SignedIn ? _account.Me : null;
        if (err != "") Surface.Community.Error = "Offline: " + err;
        Surface.InvalidateVisual();
        _ = AccountLoadGsp(root);   // (not awaited: Play waits for the session only, never for the lobbies' answers)
    }

    /// <summary>a lobby ticket now (from the service; else the one written last while still fresh); null: none</summary>
    async Task<string?> AccountTicket(string root)
    {
        if (_account == null || !_account.SignedIn) return null;
        try { return await _account.LobbyTicketAsync(); }
        catch (AccountException e) { Updater.Log(root, "leaderboard: no lobby ticket now: " + e.Message); return LobbyBoard.StoredTicket(root); }
    }

    /// <summary>the account's GSP, places, row and shown choice from the official lobbies, by the ticket</summary>
    async Task AccountLoadGsp(string root)
    {
        if (!AccountMode || _account == null || !_account.SignedIn) return;
        string? ticket = await AccountTicket(root);
        if (string.IsNullOrEmpty(ticket)) return;
        var me = await LobbyBoard.MeAsync(Http, OfficialLobby.All(root), ticket, CancellationToken.None);
        if (me == null || root != _accountRoot) return;
        var c = Surface.Community;
        c.Records = me.Fighters.Where(f => f.Gsp > 0).Select(f => new FighterGsp { Fighter = f.Fighter, Gsp = f.Gsp, Rank = f.Rank }).ToList();
        c.Rank = me.Rank;
        c.FighterRanks = me.Fighters.Where(f => f.Rank > 0).ToDictionary(f => f.Fighter, f => f.Rank);
        c.BoardId = me.Id == "" ? null : me.Id;
        c.Shown = me.Shown;
        Surface.InvalidateVisual();
        _ = CommunityRefresh();   // (their own row in the top 10, by the row id the lobby gave)
    }

    internal async Task AccountAction(string id, string root)
    {
        var c = Surface.Community;
        switch (id)
        {
            case "community:signin" when !c.Busy && _account != null:
                c.Busy = true; c.Error = "";
                _discordCancel = new CancellationTokenSource();
                Surface.InvalidateVisual();
                try
                {
                    await _account.SignInWithBrowserAsync(_discordCancel.Token);
                    c.Me = _account.SignedIn ? _account.Me : null;
                }
                catch (OperationCanceledException) { c.Error = "Sign-in cancelled."; }
                catch (AccountException e)
                {
                    c.Error = e.Message;
                    Console.WriteLine($"[OpenSmash Account] Sign-in failed: {e.Message}");
                    try { File.AppendAllText(Path.Combine(root, "build", "launcher.log"), $"[OpenSmash Account] Sign-in failed: {e.Message}\n"); } catch { }
                    Updater.Log(root, "sign-in: " + e.Message);
                }
                catch (Exception e) when (e is IOException or System.Net.Sockets.SocketException or HttpRequestException or System.Text.Json.JsonException or KeyNotFoundException or InvalidOperationException or System.ComponentModel.Win32Exception)
                {
                    c.Error = "The sign-in did not work: " + e.Message;
                    Console.WriteLine($"[OpenSmash Account] Sign-in error: {e}");
                    try { File.AppendAllText(Path.Combine(root, "build", "launcher.log"), $"[OpenSmash Account] Sign-in error: {e}\n"); } catch { }
                    Updater.Log(root, "sign-in: " + e);
                }
                finally { c.Busy = false; _discordCancel?.Dispose(); _discordCancel = null; }
                Surface.InvalidateVisual();
                await AccountLoadGsp(root);
                break;
            case "community:shown" when _account != null && _account.SignedIn:
                c.Shown = !c.Shown; c.Error = "";
                Surface.InvalidateVisual();
                string? ticket = await AccountTicket(root);
                int took = ticket == null ? 0 : await LobbyBoard.SetShownAsync(Http, OfficialLobby.All(root), ticket, c.Shown, CancellationToken.None);
                if (took == 0) { c.Shown = !c.Shown; c.Error = "The lobby did not answer: try again."; }
                else _ = CommunityRefresh();
                break;
            case "community:manage":
                if (_account?.Service.AccountPage is { } page) Commands.OpenPath(page);
                break;
            case "community:signout" when _account != null:
                await _account.SignOutAsync();
                new OnlinePass(_account, root).Remove();
                c.Me = null; c.BoardId = null; c.Rank = 0; c.FighterRanks = new(); c.Records = new(); c.Open = false; c.Error = ""; c.Shown = true;
                _ = CommunityRefresh();
                break;
        }
        Surface.InvalidateVisual();
    }

    /// <summary>before the game starts: a fresh ticket in the file the game reads, and the variable naming it (null: not
    /// signed in or no ticket now - the game then says "sign in in the launcher" and stays off the official lobbies)</summary>
    internal async Task<IReadOnlyDictionary<string, string>?> AccountPassForGame(string root)
    {
        if (_accountResume != null) { try { await _accountResume; } catch (Exception e) when (e is AccountException or HttpRequestException) { } }
        if (!AccountMode || _account == null) return null;
        _pass?.Dispose();
        _pass = new OnlinePass(_account, root) { Log = s => Updater.Log(root, s) };
        if (!_account.SignedIn) { _pass.Remove(); return null; }
        return await _pass.WriteAsync() ? new Dictionary<string, string> { ["SSBU_ONLINE_TICKET_FILE"] = _pass.File } : null;
    }

    /// <summary>the game's window opened: the ticket kept fresh while it runs</summary>
    void AccountGameStarted()
    {
        if (_pass != null && File.Exists(_pass.File)) _pass.KeepFresh();
    }

    /// <summary>the game exited: no more renewal; the GSP it earned read again</summary>
    internal void AccountGameEnded(string root)
    {
        _pass?.Dispose(); _pass = null;
        if (AccountMode && _backgroundActions) _ = AccountLoadGsp(root);
    }
}
