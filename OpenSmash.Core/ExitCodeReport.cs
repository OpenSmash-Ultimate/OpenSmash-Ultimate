// SPDX-License-Identifier: GPL-3.0-or-later
// After a game that ended abnormally without writing a report: a crash report made from its exit code (v0.5.5).
// The game's own handler (runtime/host.c on_fault, a vectored exception handler) writes reports\crash_*.txt for the
// faults it can catch: an access violation, an illegal instruction, a stack overflow it survives, a division by zero,
// an in-page error. What bypasses it leaves no file, so the launcher (CrashReport.SendIfAnyAsync) sent nothing: a fast
// fail (0xC0000409: __fastfail, the C runtime's and drivers' checks, Control Flow Guard), heap corruption
// (0xC0000374), a stack overflow the handler's thread cannot report, a C++ exception or breakpoint nothing handled, a
// driver ending the process. 0.5.4's field data had players' play.logs that end mid-fight, with nothing sent.
// Now, when the game's exit code says it crashed and no report was written in this run, the launcher sends one: kind
// "crash", its first line "exit code 0xXXXXXXXX (NAME)", with the end of play.log, by the same upload, size caps and
// scrubbing (CrashReport.Clean) as the game's own reports. A normal quit never reports: exit code 0, the game's own
// codes (2, 3, 4: it stopped by itself and said why; runtime/host.c rt_fatal writes its stopped_*.txt), and an end from
// outside the game (0xFFFFFFFF: Process.Kill, TerminateProcess(-1); 1: taskkill, Task Manager; Ctrl+C; a debugger).
// 5 and 6 are the runtime's own crash exits (its handler, a fault while reporting one): reported only when no report
// file came with them. On Linux and macOS, a death by a crash signal (exit code 128 + the signal: SIGILL, SIGTRAP,
// SIGABRT, SIGBUS, SIGFPE, SIGSEGV); SIGKILL and SIGTERM are ends from outside (the system's out-of-memory killer
// included: the log says so) and are not reported.
// A game started again from its settings (runtime/hle_settings.c settings_restart) is another process: the launcher
// has the first one's exit code only (0), so a crash of the restarted game without a report is still not sent.
// OPENSMASH_EXIT_REPORT=0 (or off): no report from the exit code, as before. The game reads it too (runtime/host.c:
// play.log's flush once a second, the last-chance line).
using System;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

namespace OpenSmash.Core
{
    public static class ExitCodeReport
    {
        public const string Variable = "OPENSMASH_EXIT_REPORT";
        /// <summary>bytes of play.log's end put in the report itself (the log goes too, as with every report)</summary>
        public const int TailBytes = 16 * 1024;

        /// <summary>OPENSMASH_EXIT_REPORT: on unless 0 or off</summary>
        public static bool On(string value)
        {
            string v = (value ?? "").Trim();
            return !(v == "0" || v.Equals("off", StringComparison.OrdinalIgnoreCase));
        }

        /// <summary>the symbolic name of a Windows exit code (an NTSTATUS, or the runtime's own); null when not known</summary>
        public static string Name(uint code) => code switch
        {
            5 => "the game's crash handler, no report file",
            6 => "a fault while a crash was reported",
            0xC0000005 => "STATUS_ACCESS_VIOLATION",
            0xC0000006 => "STATUS_IN_PAGE_ERROR",
            0xC0000008 => "STATUS_INVALID_HANDLE",
            0xC0000017 => "STATUS_NO_MEMORY",
            0xC000001D => "STATUS_ILLEGAL_INSTRUCTION",
            0xC0000025 => "STATUS_NONCONTINUABLE_EXCEPTION",
            0xC0000094 => "STATUS_INTEGER_DIVIDE_BY_ZERO",
            0xC0000096 => "STATUS_PRIVILEGED_INSTRUCTION",
            0xC00000FD => "STATUS_STACK_OVERFLOW",
            0xC0000135 => "STATUS_DLL_NOT_FOUND",
            0xC0000139 => "STATUS_ENTRYPOINT_NOT_FOUND",
            0xC0000142 => "STATUS_DLL_INIT_FAILED",
            0xC000013A => "STATUS_CONTROL_C_EXIT",
            0xC0000374 => "STATUS_HEAP_CORRUPTION",
            0xC0000409 => "STATUS_STACK_BUFFER_OVERRUN",
            0xC0000417 => "STATUS_INVALID_CRUNTIME_PARAMETER",
            0xC000041D => "STATUS_FATAL_USER_CALLBACK_EXCEPTION",
            0xC0000420 => "STATUS_ASSERTION_FAILURE",
            0xC0000602 => "STATUS_FAIL_FAST_EXCEPTION",
            0xC06D007E => "a delay-loaded DLL not found",
            0xC06D007F => "a delay-loaded function not found",
            0xE06D7363 => "an unhandled C++ exception",
            0x80000003 => "STATUS_BREAKPOINT",
            0x40000015 => "STATUS_FATAL_APP_EXIT",
            0x40010004 => "DBG_TERMINATE_PROCESS",
            0xFFFFFFFF => "ended from outside (TerminateProcess -1)",
            _ => null,
        };

        /// <summary>what the code means, in a few words, for the report's second line; null when nothing to add</summary>
        static string Meaning(uint code) => code switch
        {
            0xC0000409 => "a fast fail (__fastfail): a check of the C runtime, a driver or Control Flow Guard ended the process at once; no handler runs",
            0xC0000374 => "the heap was found corrupted: something wrote outside its memory earlier",
            0xC00000FD => "a stack overflow the game's handler could not report",
            0xC0000005 => "an access violation the game's handler did not catch (its own thread failed, or another handler ended the process)",
            0xE06D7363 => "a C++ exception nothing caught (a driver's or a library's)",
            0x40000015 => "abort() was called",
            0xC0000420 => "an assertion failed",
            0xC0000602 => "a fail-fast exception",
            0xC0000017 => "out of memory",
            0xC0000135 or 0xC0000139 or 0xC0000142 or 0xC06D007E or 0xC06D007F => "a library the game needs did not load",
            5 => "the crash handler ran (see the end of play.log) but its report file was not written",
            6 => "a second fault while the first was being reported",
            _ => null,
        };

        static readonly int[] CrashSignals = { 4, 5, 6, 7, 8, 11 };   // SIGILL, SIGTRAP, SIGABRT, SIGBUS (Linux), SIGFPE, SIGSEGV

        static string SignalName(int sig, bool mac) => sig switch
        {
            4 => "SIGILL", 5 => "SIGTRAP", 6 => "SIGABRT", 7 => mac ? "SIGEMT" : "SIGBUS", 8 => "SIGFPE", 9 => "SIGKILL",
            10 => mac ? "SIGBUS" : "SIGUSR1", 11 => "SIGSEGV", 15 => "SIGTERM", _ => null,
        };

        /// <summary>whether an exit code says the game crashed (and so a report is wanted when it wrote none). windows: the
        /// code is a Windows one (else 128 + a signal for a death by one; mac: macOS's signal numbers)</summary>
        public static bool Abnormal(int code, bool windows, bool mac = false)
        {
            if (code == 5 || code == 6) return true;   // (the runtime's crash exits: only reported when no report file came)
            if (windows)
            {
                uint u = unchecked((uint)code);
                if (u < 0x40000000u) return false;   // (0, the game's own codes, taskkill's and Task Manager's 1)
                return u != 0xFFFFFFFFu && u != 0xC000013Au && u != 0x40010004u;   // (Kill / TerminateProcess(-1), Ctrl+C, a debugger)
            }
            int sig = code - 128;
            return CrashSignals.Contains(sig) && !(mac && sig == 7) || (mac && sig == 10);
        }

        /// <summary>"exit code 0xC0000409 (STATUS_STACK_BUFFER_OVERRUN)": the report's first line and title</summary>
        public static string Describe(int code, bool windows, bool mac = false)
        {
            if (!windows && code > 128 && code < 128 + 65)
            {
                string s = SignalName(code - 128, mac);
                return "exit code " + code.ToString(CultureInfo.InvariantCulture) + " (signal " + (code - 128).ToString(CultureInfo.InvariantCulture) + (s != null ? " " + s : "") + ")";
            }
            uint u = unchecked((uint)code);
            string name = windows ? Name(u) : null;
            return "exit code 0x" + u.ToString("X8", CultureInfo.InvariantCulture) + (name != null ? " (" + name + ")" : "");
        }

        /// <summary>the report's text: its first line (Describe), what it means, and the end of play.log (logPath; none if
        /// missing), whole lines within TailBytes. Cleaned by the caller.</summary>
        public static string Text(int code, bool windows, bool mac, string logPath)
        {
            var sb = new StringBuilder();
            sb.Append(Describe(code, windows, mac)).Append('\n');
            string meaning = windows ? Meaning(unchecked((uint)code)) : null;
            if (meaning != null) sb.Append(meaning).Append('\n');
            sb.Append("The game ended without writing a crash report: this one is the launcher's, from the exit code (").Append(Variable).Append("=0: none).\n");
            string tail = null;
            try { if (!string.IsNullOrEmpty(logPath) && File.Exists(logPath)) tail = CrashReport.Tail(logPath, TailBytes); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            if (tail != null)
            {
                int nl = tail.IndexOf('\n');
                if (new FileInfo(logPath).Length > TailBytes && nl >= 0 && nl + 1 < tail.Length) tail = tail.Substring(nl + 1);   // (whole lines)
                sb.Append("\nThe end of play.log:\n").Append(tail);
                if (!tail.EndsWith("\n", StringComparison.Ordinal)) sb.Append('\n');
            }
            else sb.Append("\n(no play.log)\n");
            return sb.ToString();
        }
    }
}
