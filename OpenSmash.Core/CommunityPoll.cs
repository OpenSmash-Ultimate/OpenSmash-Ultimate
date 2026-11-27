// SPDX-License-Identifier: GPL-3.0-or-later
// How often the launchers ask the accounts service (Supabase) for what the home page shows of everyone: the top mastery
// (opensmash_mastery_board), the region's players online (lobby_status) and SSBU Arena's ranked board (ranked_board).
// Thousands of launchers stay open for hours, and each asked every minute; those numbers change slowly, so they are asked
// at most every `Interval` (5 minutes), only while the launcher's window is shown (not minimized, not hidden while the
// game runs), once at start, and once again when the window is shown after more than `Interval`. A random wait of up to
// 30 s is added to each interval so the launchers started together do not keep asking in the same second.
// The lobbies' own pages (status.json, leaderboard.json) are not the accounts service: they keep their minute.
// OPENSMASH_COMMUNITY_POLL_S=<seconds> (30..3600, default 300); 60 is the old pace (and no random wait at 60 or below).
// The top mastery is asked less often than the rest: the accounts service keeps that board's answer for an hour
// (private.opensmash_board_cache_cfg ttl_s 3600), so asking it every 5 minutes got the same rows 11 times in 12. It is asked
// at start and then when its last answer is `BoardInterval` old (30 minutes), at the next poll; a failed ask is tried again
// at the next poll. OPENSMASH_BOARD_POLL_S=<seconds> (300..3600, default 1800); 0 = at every poll, as before.
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

        public const string BoardVariable = "OPENSMASH_BOARD_POLL_S";
        public const int BoardDefaultSeconds = 1800, BoardMinSeconds = 300;

        /// <summary>how old the top mastery's answer may get before it is asked again (OPENSMASH_BOARD_POLL_S): none or not a
        /// number: 30 minutes; 0: TimeSpan.Zero, asked at every poll (the old way)</summary>
        public static TimeSpan ParseBoard(string value)
        {
            if (!int.TryParse((value ?? "").Trim(), System.Globalization.NumberStyles.Integer, System.Globalization.CultureInfo.InvariantCulture, out int s) || s < 0)
                s = BoardDefaultSeconds;
            return s == 0 ? TimeSpan.Zero : TimeSpan.FromSeconds(Math.Clamp(s, BoardMinSeconds, MaxSeconds));
        }

        public static TimeSpan BoardInterval => ParseBoard(Environment.GetEnvironmentVariable(BoardVariable));

        /// <summary>whether this poll asks the top mastery too: never asked (or the last ask failed), the old way (Zero), or the
        /// last answer at least the board interval old less 5 s (the poll's own timer lands about on it)</summary>
        public static bool BoardDue(DateTime lastUtc, DateTime nowUtc, TimeSpan boardInterval) =>
            lastUtc == DateTime.MinValue || boardInterval <= TimeSpan.Zero || nowUtc - lastUtc >= boardInterval - TimeSpan.FromSeconds(5);

        /// <summary>whether the window shown again should ask now: never asked, or the last answer older than the interval</summary>
        public static bool Due(DateTime lastUtc, DateTime nowUtc, TimeSpan interval) =>
            lastUtc == DateTime.MinValue || nowUtc - lastUtc >= interval;

        /// <summary>a page asked again when it is opened (the COMPETITIVE board): fresh enough when younger than the
        /// interval less 5 s (so the timer's own ask, at the interval, is never skipped)</summary>
        public static bool Fresh(DateTime lastUtc, DateTime nowUtc, TimeSpan interval) =>
            lastUtc != DateTime.MinValue && nowUtc - lastUtc < interval - TimeSpan.FromSeconds(5);
    }
}
