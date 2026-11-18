// SPDX-License-Identifier: GPL-3.0-or-later
// Release versions: "0.5.0", "v0.5.0", "0.5.1-rc.1" (Semantic Versioning 2.0 precedence; build metadata after '+' ignored).
using System;
using System.Linq;

namespace OpenSmash.Core
{
    public sealed class SemVer : IComparable<SemVer>
    {
        public int Major, Minor, Patch;
        public string Pre = "";     // "" for a release; "rc.1", "alpha.3"...

        public static SemVer Parse(string s) => TryParse(s, out var v) ? v : throw new FormatException("not a version: " + s);

        public static bool TryParse(string s, out SemVer v)
        {
            v = null;
            if (string.IsNullOrWhiteSpace(s)) return false;
            s = s.Trim();
            if (s.StartsWith("v", StringComparison.OrdinalIgnoreCase)) s = s.Substring(1);
            int plus = s.IndexOf('+');
            if (plus >= 0) s = s.Substring(0, plus);
            string pre = "";
            int dash = s.IndexOf('-');
            if (dash >= 0) { pre = s.Substring(dash + 1); s = s.Substring(0, dash); if (pre.Length == 0) return false; }
            var p = s.Split('.');
            if (p.Length < 2 || p.Length > 4) return false;     // (4: a .NET assembly version "0.5.0.0")
            var n = new int[3];
            for (int i = 0; i < p.Length; i++)
            {
                if (!int.TryParse(p[i], System.Globalization.NumberStyles.None, null, out int x)) return false;
                if (i < 3) n[i] = x;
            }
            v = new SemVer { Major = n[0], Minor = n[1], Patch = n[2], Pre = pre };
            return true;
        }

        public int CompareTo(SemVer o)
        {
            if (o == null) return 1;
            int c = Major.CompareTo(o.Major);
            if (c == 0) c = Minor.CompareTo(o.Minor);
            if (c == 0) c = Patch.CompareTo(o.Patch);
            if (c != 0) return c;
            if (Pre == o.Pre) return 0;
            if (Pre == "") return 1;          // a release is newer than its pre-releases
            if (o.Pre == "") return -1;
            var a = Pre.Split('.'); var b = o.Pre.Split('.');
            for (int i = 0; i < Math.Min(a.Length, b.Length); i++)
            {
                bool an = int.TryParse(a[i], out int ai), bn = int.TryParse(b[i], out int bi);
                if (an && bn) c = ai.CompareTo(bi);
                else if (an) c = -1;
                else if (bn) c = 1;
                else c = string.CompareOrdinal(a[i], b[i]);
                if (c != 0) return Math.Sign(c);
            }
            return a.Length.CompareTo(b.Length);
        }

        public bool IsPreRelease => Pre != "";
        public override string ToString() => $"{Major}.{Minor}.{Patch}" + (Pre != "" ? "-" + Pre : "");
        public static bool operator >(SemVer a, SemVer b) => a != null && a.CompareTo(b) > 0;
        public static bool operator <(SemVer a, SemVer b) => b != null && b.CompareTo(a) > 0;
        public override bool Equals(object obj) => obj is SemVer o && CompareTo(o) == 0;
        public override int GetHashCode() => ToString().GetHashCode();
        internal static bool AllDigits(string s) => s.Length > 0 && s.All(char.IsDigit);
    }
}
