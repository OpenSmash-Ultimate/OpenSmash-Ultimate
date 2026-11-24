// SPDX-License-Identifier: GPL-3.0-or-later
// The server region: OpenSmash's official lobbies, one a region, and which of them this install plays on.
// The same setup and the same accounts on each (one lobby ticket works on all), but rooms and matchmaking are a
// lobby's own: players on another region are not listed.
// The game's copy of the table and of the rules is runtime/net.h's NET_REGION_TABLE and runtime/net_region.c
// (tests/test_launcher_defaults.py compares the tables); the files are the same ones:
//   config/server_region.txt        "auto" (the default: the region whose lobby answers soonest) or a region's id
//   config/server_region_auto.txt   Auto's last pick, "<id> <ms> <unix time>": the game starts on it without waiting
// An override (OfficialLobby.Override: SSBU_OFFICIAL_LOBBY, config/official_lobby.txt) is that server: no regions.
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Threading.Tasks;

namespace OpenSmash.Core
{
    public sealed class ServerRegion
    {
        public string Id { get; }
        public string Label { get; }
        /// <summary>host:port, a DNS name (never an address in numbers)</summary>
        public string Lobby { get; }
        public string Host => Lobby.Substring(0, Lobby.LastIndexOf(':'));
        public int Port => int.Parse(Lobby.Substring(Lobby.LastIndexOf(':') + 1), CultureInfo.InvariantCulture);

        ServerRegion(string id, string label, string lobby) { Id = id; Label = label; Lobby = lobby; }

        /// <summary>the first is the fallback (no answer from any)</summary>
        public static readonly ServerRegion[] All =
        {
            new ServerRegion("eu", "EU", OfficialLobby.Default),
            new ServerRegion("na-east", "NA East", "na.lobby.opensmashultimate.com:39421"),
        };

        public const string Auto = "auto";
        public const string SettingFile = "server_region.txt", AutoFile = "server_region_auto.txt";
        public const int Samples = 3, SampleWaitMs = 350, WaitMs = 2500;
        /// <summary>Auto stays on its last pick unless another region answers this much sooner (two close regions: no hopping)</summary>
        public const int KeepMarginMs = 10;

        public static ServerRegion ById(string id) =>
            All.FirstOrDefault(r => string.Equals(r.Id, (id ?? "").Trim(), StringComparison.OrdinalIgnoreCase));

        /// <summary>the region whose lobby this is ("host:port", or the host alone)</summary>
        public static ServerRegion OfLobby(string lobby) =>
            string.IsNullOrEmpty(lobby) ? null : All.FirstOrDefault(r => string.Equals(r.Lobby, lobby, StringComparison.OrdinalIgnoreCase) || string.Equals(r.Host, lobby, StringComparison.OrdinalIgnoreCase));

        static string FirstWord(string file)
        {
            try
            {
                if (!File.Exists(file)) return "";
                string line = File.ReadLines(file).Select(l => l.Trim()).FirstOrDefault(l => l != "" && !l.StartsWith("#")) ?? "";
                return line.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries).FirstOrDefault() ?? "";
            }
            catch (IOException) { return ""; }
            catch (UnauthorizedAccessException) { return ""; }
        }

        /// <summary>the saved choice: "auto" or a region's id (no file, no install, a word that is no region: auto)</summary>
        public static string Setting(string root) =>
            string.IsNullOrEmpty(root) ? Auto : ById(FirstWord(Path.Combine(root, "config", SettingFile)))?.Id ?? Auto;

        /// <summary>writes the choice (the file's comments kept, as the game's settings menu writes it)</summary>
        public static void SetSetting(string root, string value)
        {
            value = ById(value)?.Id ?? Auto;
            string file = Path.Combine(root, "config", SettingFile);
            Directory.CreateDirectory(Path.GetDirectoryName(file));
            var lines = File.Exists(file) ? File.ReadAllLines(file).ToList() : new List<string>();
            int at = lines.FindIndex(l => l.Trim() != "" && !l.Trim().StartsWith("#"));
            if (at >= 0) lines[at] = value; else lines.Add(value);
            File.WriteAllLines(file, lines);
        }

        // ---- the pick ---------------------------------------------------------------------------------------------
        /// <summary>the median of the samples that are >= 0; -1: none</summary>
        public static int Median(IEnumerable<int> ms)
        {
            var v = (ms ?? Enumerable.Empty<int>()).Where(x => x >= 0).OrderBy(x => x).ToArray();
            return v.Length == 0 ? -1 : v.Length % 2 == 1 ? v[v.Length / 2] : (v[v.Length / 2 - 1] + v[v.Length / 2]) / 2;
        }

        /// <summary>the region with the lowest time (ms[i] of All[i]; under 0: no answer), the earlier of two the same;
        /// `kept` (Auto's last pick) stays unless another answers KeepMarginMs sooner. null: none answered.</summary>
        public static ServerRegion Pick(IReadOnlyList<int> ms, ServerRegion kept = null)
        {
            int best = -1;
            for (int i = 0; i < All.Length && i < ms.Count; i++)
                if (ms[i] >= 0 && (best < 0 || ms[i] < ms[best])) best = i;
            if (best < 0) return null;
            int k = kept == null ? -1 : Array.IndexOf(All, kept);
            if (k >= 0 && k < ms.Count && ms[k] >= 0 && ms[k] - ms[best] < KeepMarginMs) return kept;
            return All[best];
        }

        /// <summary>config/server_region_auto.txt's line: the region (null: not one) and its time (-1: none)</summary>
        public static (ServerRegion Region, int Ms) ParseKept(string line)
        {
            var w = (line ?? "").Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
            int ms = w.Length > 1 && int.TryParse(w[1], NumberStyles.None, CultureInfo.InvariantCulture, out int t) && t < 60000 ? t : -1;
            return (w.Length > 0 ? ById(w[0]) : null, ms);
        }

        public static (ServerRegion Region, int Ms) Kept(string root)
        {
            if (string.IsNullOrEmpty(root)) return (null, -1);
            try
            {
                string f = Path.Combine(root, "config", AutoFile);
                return File.Exists(f) ? ParseKept(File.ReadLines(f).FirstOrDefault() ?? "") : (null, -1);
            }
            catch (IOException) { return (null, -1); }
            catch (UnauthorizedAccessException) { return (null, -1); }
        }

        static void Keep(string root, ServerRegion r, int ms)
        {
            if (string.IsNullOrEmpty(root) || !Directory.Exists(root)) return;
            try
            {
                Directory.CreateDirectory(Path.Combine(root, "config"));
                File.WriteAllText(Path.Combine(root, "config", AutoFile),
                    r.Id + " " + ms.ToString(CultureInfo.InvariantCulture) + " " + DateTimeOffset.UtcNow.ToUnixTimeSeconds().ToString(CultureInfo.InvariantCulture) + "\n");
            }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }

        // ---- measuring --------------------------------------------------------------------------------------------
        /// <summary>One region's lobby asked `samples` times: each answer's time in ms (none: no answer). Tests put their
        /// own here; the default asks the lobby over UDP (Ask).</summary>
        public static Func<ServerRegion, int, int[]> Asker = Ask;

        /// <summary>The question is a JOIN of netplay version 2 with no lobby ticket ("SN", version, type 1, a nonce,
        /// the lobby code, the name "ping", no build): an official lobby lets nobody in without one and says so at once
        /// (REJECT, type 3), which is all that is timed. A lobby that does let it in (WELCOME, 2) is left (LEAVE, 8).</summary>
        static readonly object SocketMade = new object();

        public static int[] Ask(ServerRegion region, int samples)
        {
            var got = new List<int>();
            try
            {
                var address = Dns.GetHostAddresses(region.Host).FirstOrDefault(a => a.AddressFamily == AddressFamily.InterNetwork);
                if (address == null) return got.ToArray();
                var at = new IPEndPoint(address, region.Port);
                UdpClient made;
                lock (SocketMade) made = new UdpClient(AddressFamily.InterNetwork);   // (#542: .NET 8 on macOS can crash when two threads make a socket at once)
                using var udp = made;
                byte[] join = new byte[4 + 8 + 32 + 32 + 40];
                join[0] = (byte)'S'; join[1] = (byte)'N'; join[2] = 2; join[3] = 1;
                byte[] nonce = Guid.NewGuid().ToByteArray().Take(8).ToArray();
                nonce.CopyTo(join, 4);
                System.Text.Encoding.ASCII.GetBytes("smash").CopyTo(join, 12);
                System.Text.Encoding.ASCII.GetBytes("ping").CopyTo(join, 12 + 32);
                for (int i = 0; i < samples; i++)
                {
                    var clock = Stopwatch.StartNew();
                    udp.Send(join, join.Length, at);
                    bool answered = false;
                    while (!answered && clock.ElapsedMilliseconds < SampleWaitMs)
                    {
                        udp.Client.ReceiveTimeout = Math.Max(1, SampleWaitMs - (int)clock.ElapsedMilliseconds);
                        var from = new IPEndPoint(IPAddress.Any, 0);
                        byte[] p;
                        try { p = udp.Receive(ref from); }
                        catch (SocketException) { break; }   // (no answer in time, "port unreachable")
                        if (p.Length < 12 || !from.Address.Equals(address) || p[0] != 'S' || p[1] != 'N' || !p.Skip(4).Take(8).SequenceEqual(nonce)) continue;
                        if (p[3] != 2 && p[3] != 3) continue;
                        answered = true;
                        got.Add((int)Math.Round(clock.Elapsed.TotalMilliseconds));
                        if (p[3] == 2)
                        {
                            byte[] leave = new byte[12];
                            leave[0] = (byte)'S'; leave[1] = (byte)'N'; leave[2] = 2; leave[3] = 8;
                            nonce.CopyTo(leave, 4);
                            udp.Send(leave, leave.Length, at);
                            i = samples;
                        }
                    }
                    if (!answered && got.Count == 0 && i > 0) break;   // (nothing twice: not there)
                }
            }
            catch (SocketException) { }   // (a name that is not found, no network)
            catch (ObjectDisposedException) { }
            catch (ArgumentException) { }
            return got.ToArray();
        }

        /// <summary>every region asked at once, at most waitMs in all: the median of each (All's order; -1 no answer)</summary>
        public static async Task<int[]> MeasureAll(int waitMs = WaitMs)
        {
            var asker = Asker;
            var asks = All.Select(r => Task.Run(() => { try { return Median(asker(r, Samples)); } catch (Exception) { return -1; } })).ToArray();
            await Task.WhenAny(Task.WhenAll(asks), Task.Delay(waitMs)).ConfigureAwait(false);
            return asks.Select(t => t.Status == TaskStatus.RanToCompletion ? t.Result : -1).ToArray();
        }

        // (what was measured in this process, where no file can keep it: an install not there yet)
        static readonly ConcurrentDictionary<string, (ServerRegion Region, int Ms)> Measured = new ConcurrentDictionary<string, (ServerRegion, int)>(StringComparer.OrdinalIgnoreCase);

        /// <summary>Measures and keeps Auto's pick for the install in root (config/server_region_auto.txt, which the
        /// game starts from). The region and its time; (null, -1): no lobby answered (the pick kept before stays).</summary>
        public static async Task<(ServerRegion Region, int Ms)> Measure(string root, int waitMs = WaitMs)
        {
            int[] ms = await MeasureAll(waitMs).ConfigureAwait(false);
            var pick = Pick(ms, AutoPick(root).Region);
            if (pick == null) return (null, -1);
            int t = ms[Array.IndexOf(All, pick)];
            Measured[root ?? ""] = (pick, t);
            Keep(root, pick, t);
            return (pick, t);
        }

        /// <summary>Auto's pick as it stands: the one kept in the install (the game's too), else this process's own;
        /// (null, -1): not measured yet</summary>
        public static (ServerRegion Region, int Ms) AutoPick(string root)
        {
            var kept = Kept(root);
            if (kept.Region != null) return kept;
            return Measured.TryGetValue(root ?? "", out var m) ? m : (null, -1);
        }

        /// <summary>the region this install plays on (null: an override's server, no regions); Auto with nothing measured: the first</summary>
        public static ServerRegion Current(string root)
        {
            if (OfficialLobby.Override(root) != null) return null;
            return ById(Setting(root)) ?? AutoPick(root).Region ?? All[0];
        }

        /// <summary>the choice as it is shown: "Auto (EU, 24 ms)", "Auto (EU)", "Auto", "NA East"; an override: its server</summary>
        public static string Shown(string root)
        {
            string other = OfficialLobby.Override(root);
            if (other != null) return other;
            var chosen = ById(Setting(root));
            if (chosen != null) return chosen.Label;
            var (r, ms) = AutoPick(root);
            return r == null ? "Auto" : ms >= 0 ? "Auto (" + r.Label + ", " + ms.ToString(CultureInfo.InvariantCulture) + " ms)" : "Auto (" + r.Label + ")";
        }
    }
}
