// The players online in one region, from the accounts service's public table lobby_status: one row a product and
// region (product 'opensmash', region 'eu' / 'na-east' / 'sa': ServerRegion's ids), which each region's lobby keeps up to
// date. Read with the publishable key over HTTPS (Account.AnonAsync), so the launcher's home page has the count of the
// selected (or Auto's) region even where the lobby's own plain-HTTP status page cannot be reached.
//
// A row older than 15 minutes is "unknown" (its lobby stopped reporting): no count, as the site does. No row for the
// region (a database that only has the older single row), no accounts service, or anything unreadable: null, and the
// launcher shows what it showed before, the lobby's own /status.json. OPENSMASH_ONLINE_COUNT=lobby: only that.
using System;
using System.Globalization;
using System.Text.Json;

namespace OpenSmash.Core
{
    public static class OnlineCount
    {
        public const string Product = "opensmash";
        public static readonly TimeSpan Stale = TimeSpan.FromMinutes(15);

        /// <summary>whether the table is asked at all (OPENSMASH_ONLINE_COUNT=lobby: the lobby's own status page only)</summary>
        public static bool Wanted =>
            !string.Equals(Environment.GetEnvironmentVariable("OPENSMASH_ONLINE_COUNT"), "lobby", StringComparison.OrdinalIgnoreCase);

        /// <summary>the REST path of a region's row (for Account.AnonAsync)</summary>
        public static string Path(string region) =>
            "/rest/v1/lobby_status?select=players,rooms,updated_at&product=eq." + Product + "&region=eq." + Uri.EscapeDataString(region ?? "") + "&limit=1";

        /// <summary>the row's players and rooms, or null: no row, not this table's answer, or a row not updated for
        /// `Stale` (or dated in the future by more than that)</summary>
        public static (int Players, int Rooms)? Parse(string json, DateTimeOffset now)
        {
            try
            {
                using var j = JsonDocument.Parse(json);
                var r = j.RootElement;
                if (r.ValueKind != JsonValueKind.Array || r.GetArrayLength() == 0) return null;
                var row = r[0];
                if (row.ValueKind != JsonValueKind.Object) return null;
                if (!row.TryGetProperty("players", out var p) || p.ValueKind != JsonValueKind.Number || !p.TryGetInt32(out int players)) return null;
                if (!row.TryGetProperty("updated_at", out var u) || u.ValueKind != JsonValueKind.String
                    || !DateTimeOffset.TryParse(u.GetString(), CultureInfo.InvariantCulture, DateTimeStyles.AssumeUniversal, out var at)) return null;
                if ((now - at).Duration() > Stale) return null;
                int rooms = row.TryGetProperty("rooms", out var m) && m.ValueKind == JsonValueKind.Number && m.TryGetInt32(out int n) ? n : 0;
                return (Math.Max(0, players), Math.Max(0, rooms));
            }
            catch (Exception e) when (e is JsonException || e is InvalidOperationException) { return null; }
        }
    }
}
