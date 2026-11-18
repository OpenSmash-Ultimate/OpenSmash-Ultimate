// SPDX-License-Identifier: GPL-3.0-or-later
// OpenSmash releases: the launcher is the only download, and it follows a release channel instead of the main branch.
// Every release (tools/release/make_release.py, .github/workflows/release.yml) carries the asset opensmash-release.json:
//   {"schema": 1, "version": "0.5.0", "commit": "<40 hex>", "channel": "stable",
//    "launcher": {"windows-x64": {"file": "OpenSmash.exe", "sha256": "..."}},
//    "runtime": {"windows-avx2": {"file": "opensmash-runtime-windows-avx2.zip", "sha256": "..."}, ...},
//    "lobby": "<official lobby host:port>", "min_client_level": 1}
// Channels: "stable" = GitHub's latest release (never a pre-release); "testing" = the newest release, pre-releases too.
// A developer's git checkout follows its own branch and is never changed (the installer's own updater does that).
// Tests: OPENSMASH_RELEASE_DIR=<folder> reads opensmash-release.json and its files from a folder instead of GitHub.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace OpenSmash.Core
{
    public sealed class ReleaseFile
    {
        public string File, Sha256, Url;
    }

    public sealed class Release
    {
        public string Tag, Page, Commit, Channel, Lobby;
        public SemVer Version;
        public bool PreRelease;
        public int MinClientLevel;
        public Dictionary<string, ReleaseFile> Launcher = new Dictionary<string, ReleaseFile>();   // by platform: "windows-x64"...
        public Dictionary<string, ReleaseFile> Runtime = new Dictionary<string, ReleaseFile>();    // by platform and ISA
        /// <summary>the source archive of the release's commit (the first install and the game's updates)</summary>
        public string SourceZipUrl => "https://codeload.github.com/" + Releases.Repo + "/zip/" + Commit;
    }

    public static class Releases
    {
        public const string ManifestName = "opensmash-release.json";
        /// <summary>the repository of the releases (OPENSMASH_REPO: a fork's, for tests)</summary>
        public static string Repo => Env("OPENSMASH_REPO") ?? "OpenSmash-Ultimate/OpenSmash-Ultimate";
        public static readonly string[] Channels = { "stable", "testing" };

        static string Env(string n) { string v = Environment.GetEnvironmentVariable(n); return string.IsNullOrWhiteSpace(v) ? null : v.Trim(); }

        static readonly Lazy<HttpClient> client = new Lazy<HttpClient>(() =>
        {
            var c = new HttpClient { Timeout = TimeSpan.FromMinutes(15) };
            c.DefaultRequestHeaders.UserAgent.ParseAdd("opensmash-launcher/" + Updater.Current);
            return c;
        });
        internal static HttpClient Http => client.Value;

        /// <summary>the bytes at url: https, or a local file (OPENSMASH_RELEASE_DIR's)</summary>
        public static async Task<byte[]> GetAsync(string url, CancellationToken cancel = default, string accept = null, IProgress<double> progress = null)
        {
            if (Uri.TryCreate(url, UriKind.Absolute, out var u) && u.IsFile) return await File.ReadAllBytesAsync(u.LocalPath, cancel);
            using var rq = new HttpRequestMessage(HttpMethod.Get, url);
            if (accept != null) rq.Headers.Accept.ParseAdd(accept);
            using var rs = await Http.SendAsync(rq, HttpCompletionOption.ResponseHeadersRead, cancel);
            rs.EnsureSuccessStatusCode();
            long total = rs.Content.Headers.ContentLength ?? -1;
            using var s = await rs.Content.ReadAsStreamAsync(cancel);
            using var m = new MemoryStream();
            var buf = new byte[1 << 16];
            int n;
            while ((n = await s.ReadAsync(buf, cancel)) > 0)
            {
                m.Write(buf, 0, n);
                if (total > 0) progress?.Report((double)m.Length / total);
            }
            return m.ToArray();
        }

        public static string Sha256(byte[] b) => Convert.ToHexString(SHA256.HashData(b)).ToLowerInvariant();

        /// <summary>a release file, checked against its SHA-256 (throws when it does not match: never used then)</summary>
        public static async Task<byte[]> DownloadCheckedAsync(ReleaseFile f, CancellationToken cancel = default, IProgress<double> progress = null)
        {
            if (f?.Url == null || string.IsNullOrEmpty(f.Sha256)) throw new InvalidDataException("the release does not list this file");
            byte[] b = await GetAsync(f.Url, cancel, null, progress);
            if (!string.Equals(Sha256(b), f.Sha256, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException(f.File + ": the download does not match its SHA-256, not used");
            return b;
        }

        /// <summary>the newest release of a channel, or null (no release with a manifest yet). Throws when GitHub cannot be reached.</summary>
        public static async Task<Release> LatestAsync(string channel, CancellationToken cancel = default)
        {
            channel = channel == "testing" ? "testing" : "stable";
            string local = Env("OPENSMASH_RELEASE_DIR");
            if (local != null)
            {
                var r = ParseManifest(await File.ReadAllTextAsync(Path.Combine(local, ManifestName), cancel),
                                      name => new Uri(Path.GetFullPath(Path.Combine(local, name))).AbsoluteUri);
                r.Tag ??= "v" + r.Version; r.Page = local;
                return r;
            }
            string api = "https://api.github.com/repos/" + Repo;
            if (channel == "stable")
            {
                try { return await FromGitHubAsync(await GetJsonAsync(api + "/releases/latest", cancel), cancel); }
                catch (HttpRequestException e) when (e.StatusCode == System.Net.HttpStatusCode.NotFound) { return null; }   // (no release yet)
            }
            using var list = await GetJsonAsync(api + "/releases?per_page=15", cancel);
            Release best = null;
            foreach (var rel in list.RootElement.EnumerateArray())
            {
                if (rel.TryGetProperty("draft", out var d) && d.GetBoolean()) continue;
                Release r;
                try { r = await FromGitHubAsync(JsonDocument.Parse(rel.GetRawText()), cancel); } catch (Exception) { continue; }
                if (r != null && (best == null || r.Version > best.Version)) best = r;
            }
            return best;
        }

        static async Task<JsonDocument> GetJsonAsync(string url, CancellationToken cancel) =>
            JsonDocument.Parse(await GetAsync(url, cancel, "application/vnd.github+json"));

        static async Task<Release> FromGitHubAsync(JsonDocument rel, CancellationToken cancel)
        {
            using (rel)
            {
                var root = rel.RootElement;
                var assets = new Dictionary<string, string>(StringComparer.Ordinal);
                foreach (var a in root.GetProperty("assets").EnumerateArray())
                    assets[a.GetProperty("name").GetString()] = a.GetProperty("browser_download_url").GetString();
                if (!assets.TryGetValue(ManifestName, out var info)) return null;      // (an older release, or another kind: the runtime's, EOS's)
                var r = ParseManifest(System.Text.Encoding.UTF8.GetString(await GetAsync(info, cancel)), name => assets.TryGetValue(name, out var u) ? u : null);
                r.Tag = root.GetProperty("tag_name").GetString();
                r.Page = root.TryGetProperty("html_url", out var h) ? h.GetString() : null;
                r.PreRelease = root.TryGetProperty("prerelease", out var p) && p.GetBoolean();
                return r;
            }
        }

        /// <summary>opensmash-release.json; url(name) gives a listed file's download address</summary>
        public static Release ParseManifest(string json, Func<string, string> url)
        {
            using var d = JsonDocument.Parse(json);
            var root = d.RootElement;
            if (root.TryGetProperty("schema", out var schema) && schema.GetInt32() > 1)
                throw new InvalidDataException("this release needs a newer launcher (manifest schema " + schema.GetInt32() + ")");
            string commit = Str(root, "commit");
            if (commit == null || commit.Length != 40 || !commit.All(Uri.IsHexDigit)) throw new InvalidDataException("the release manifest has no commit");
            var r = new Release
            {
                Version = SemVer.Parse(Str(root, "version")),
                Commit = commit.ToLowerInvariant(),
                Channel = Str(root, "channel") ?? "stable",
                Lobby = Str(root, "lobby"),
                MinClientLevel = root.TryGetProperty("min_client_level", out var m) && m.ValueKind == JsonValueKind.Number ? m.GetInt32() : 0,
            };
            Files(root, "launcher", r.Launcher, url);
            Files(root, "runtime", r.Runtime, url);
            return r;
        }

        static string Str(JsonElement e, string name) => e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;

        static void Files(JsonElement root, string name, Dictionary<string, ReleaseFile> into, Func<string, string> url)
        {
            if (!root.TryGetProperty(name, out var set) || set.ValueKind != JsonValueKind.Object) return;
            foreach (var p in set.EnumerateObject())
            {
                string file = Str(p.Value, "file"), sha = Str(p.Value, "sha256");
                if (file == null || sha == null || file.Contains('/') || file.Contains('\\') || file.Contains("..")) continue;
                into[p.Name] = new ReleaseFile { File = file, Sha256 = sha.ToLowerInvariant(), Url = url(file) };
            }
        }

        /// <summary>this launcher's platform key in the manifest</summary>
        public static string Platform =>
            (OperatingSystem.IsWindows() ? "windows" : OperatingSystem.IsMacOS() ? "macos" : "linux") + "-" +
            System.Runtime.InteropServices.RuntimeInformation.OSArchitecture.ToString().ToLowerInvariant();
    }
}
