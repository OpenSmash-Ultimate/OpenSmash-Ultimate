// SPDX-License-Identifier: GPL-3.0-or-later
// How often the launchers ask the accounts service (Supabase) for what the home page shows of everyone: the top mastery
// (opensmash_mastery_board), the region's players online (lobby_status) and SSBU Arena's ranked board (ranked_board).
// Thousands of launchers stay open for hours, and each asked every minute; those numbers change slowly, so they are asked
// at most every `Interval` (5 minutes), only while the launcher's window is shown (not minimized, not hidden while the
// game runs), once at start, and once again when the window is shown after more than `Interval`. A random wait of up to
// 30 s is added to each interval so the launchers started together do not keep asking in the same second.
// The lobbies' own pages (status.json, leaderboard.json) are not the accounts service: they keep their minute.
// OPENSMASH_COMMUNITY_POLL_S=<seconds> (30..3600, default 300); 60 is the old pace (and no random wait at 60 or below).
using System;

namespace OpenSmash.Core
{
    public static class CommunityPoll
    {
        public const string Variable = "OPENSMASH_COMMUNITY_POLL_S";
        public const int DefaultSeconds = 300, MinSeconds = 30, MaxSeconds = 3600;
        public static readonly TimeSpan MaxJitter = TimeSpan.FromSeconds(30);
        static readonly Random Rng = new Random();

        /// <summary>the interval a value of OPENSMASH_COMMUNITY_POLL_S gives (none or not a number: 5 minutes)</summary>
        public static TimeSpan Parse(string value)
        {
            if (!int.TryParse((value ?? "").Trim(), System.Globalization.NumberStyles.Integer, System.Globalization.CultureInfo.InvariantCulture, out int s) || s <= 0)
                s = DefaultSeconds;
            return TimeSpan.FromSeconds(Math.Clamp(s, MinSeconds, MaxSeconds));
        }

        /// <summary>the accounts service is asked again after this long (OPENSMASH_COMMUNITY_POLL_S)</summary>
        public static TimeSpan Interval => Parse(Environment.GetEnvironmentVariable(Variable));

        /// <summary>the wait before the next ask: the interval and a random 0..30 s (none at the old pace, 60 s or less)</summary>
        public static TimeSpan Next(TimeSpan interval, double random)
        {
            if (interval <= TimeSpan.FromSeconds(60)) return interval;
            return interval + TimeSpan.FromMilliseconds(MaxJitter.TotalMilliseconds * Math.Clamp(random, 0, 1));
        }

        public static TimeSpan Next()
        {
            double r;
            lock (Rng) r = Rng.NextDouble();
            return Next(Interval, r);
        }

        /// <summary>whether the window shown again should ask now: never asked, or the last answer older than the interval</summary>
        public static bool Due(DateTime lastUtc, DateTime nowUtc, TimeSpan interval) =>
            lastUtc == DateTime.MinValue || nowUtc - lastUtc >= interval;

        /// <summary>a page asked again when it is opened (the COMPETITIVE board): fresh enough when younger than the
        /// interval less 5 s (so the timer's own ask, at the interval, is never skipped)</summary>
        public static bool Fresh(DateTime lastUtc, DateTime nowUtc, TimeSpan interval) =>
            lastUtc != DateTime.MinValue && nowUtc - lastUtc < interval - TimeSpan.FromSeconds(5);
    }
}
