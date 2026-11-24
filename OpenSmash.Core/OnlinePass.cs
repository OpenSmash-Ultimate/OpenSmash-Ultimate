// SPDX-License-Identifier: GPL-3.0-or-later
// The lobby ticket for the game: written to a file only this user can read (SSBU_ONLINE_TICKET_FILE: runtime/net_relay.c
// reads it again at most every 5 s), and renewed at half its life while the game runs (SSBU Arena 0.2.37's online pass).
using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

namespace OpenSmash.Core
{
    public sealed class OnlinePass : IDisposable
    {
        public readonly string File;
        readonly Account account;
        readonly CancellationTokenSource stop = new CancellationTokenSource();
        public Action<string> Log = _ => { };

        public OnlinePass(Account account, string root)
        {
            this.account = account;
            File = Path.Combine(root, "build", "cache", "online_pass.txt");
        }

        /// <summary>"&lt;id&gt;|&lt;name&gt;|&lt;expiry&gt;[|&lt;issued&gt;].&lt;mac&gt;": its expiry (unix seconds), 0 when not one</summary>
        public static long Expiry(string ticket)
        {
            if (string.IsNullOrEmpty(ticket)) return 0;
            int dot = ticket.LastIndexOf('.');
            var p = (dot > 0 ? ticket.Substring(0, dot) : ticket).Split('|');
            return p.Length >= 3 && long.TryParse(p[2], out long e) ? e : 0;
        }

        /// <summary>a fresh ticket in the file; false (the file removed) when not signed in or the server cannot be reached</summary>
        public async Task<bool> WriteAsync()
        {
            try
            {
                if (!account.SignedIn) { Remove(); return false; }
                string t = await account.LobbyTicketAsync();
                Directory.CreateDirectory(Path.GetDirectoryName(File));
                string tmp = File + ".tmp";
                using (System.IO.File.Create(tmp)) { }
                if (!OperatingSystem.IsWindows()) System.IO.File.SetUnixFileMode(tmp, UnixFileMode.UserRead | UnixFileMode.UserWrite);
                System.IO.File.WriteAllText(tmp, t);
                System.IO.File.Move(tmp, File, true);
                return true;
            }
            catch (Exception e) { Log("online pass not renewed: " + e.Message); return false; }
        }

        public void Remove() { try { System.IO.File.Delete(File); } catch { } }

        /// <summary>renews the ticket at half its life until Dispose (the game exited)</summary>
        public void KeepFresh()
        {
            var token = stop.Token;
            Task.Run(async () =>
            {
                while (!token.IsCancellationRequested)
                {
                    long now = DateTimeOffset.UtcNow.ToUnixTimeSeconds(), exp = 0;
                    try { exp = Expiry(System.IO.File.ReadAllText(File).Trim()); } catch { }
                    long wait = exp > now ? Math.Max((exp - now) / 2, 30) : 60;
                    try { await Task.Delay(TimeSpan.FromSeconds(Math.Min(wait, 3600)), token); } catch (OperationCanceledException) { return; }
                    await WriteAsync();   /* (half of what was left has gone by) */
                }
            }, token);
        }

        public void Dispose() { stop.Cancel(); }
    }
}
