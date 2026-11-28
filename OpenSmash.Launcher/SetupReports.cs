// SPDX-License-Identifier: GPL-3.0-or-later
// (v0.5.5) The Windows launcher's side of the setup error reports (OpenSmash.Core SetupReport.cs): a failure sent in the
// background, its error code shown at once and what became of the report when it is known; and the launcher's own crashes
// (an exception nobody caught: on the window's thread, on another thread, in a task nobody waited for), which used to end
// in .NET's own dialog or close the launcher without a trace. They are written to build\launcher_crash.log and sent as
// launcher_crash. OPENSMASH_SETUP_REPORTS=0: no report and no handler, as before. OPENSMASH_THROW_TEST=ui | thread | task
// throws one 6 s after the start (a test).
using System;
using System.IO;
using System.Linq;
using System.Net.Http;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using OpenSmash.Core;

namespace OpenSmash.Launcher
{
    static class SetupReports
    {
        /// <summary>the install's folder once known (Program sets it), for the reports and launcher_crash.log</summary>
        public static volatile string? Root;
        /// <summary>where the launcher is (a flow sets it): written with a crash</summary>
        public static volatile string? Where;

        static void Log(string s) => Updater.Log(Root, s);

        /// <summary>the line shown at once: the code, and that the report is on its way (or that reports are off)</summary>
        /// <summary>("" with OPENSMASH_SETUP_REPORTS=0: the messages as before)</summary>
        public static string FirstMessage(SetupFailure f)
        {
            if (!SetupReport.SwitchOn()) return "";
            string code = SetupReport.Code(f.Kind, f.Error, f.Root);
            return SetupReport.Enabled(f.Root) ? "Error code " + code + " — sending it to the developers…" : new SetupReportResult { Code = code, Off = true }.Message;
        }

        /// <summary>sends f in the background; show gets the first line at once (on this thread) and the result's line later
        /// (on ui's thread, when ui is still there). Never throws.</summary>
        public static void Send(SetupFailure f, Control? ui, Action<string>? show)
        {
            try
            {
                if (!SetupReport.SwitchOn()) return;   // (off: nothing sent, nothing said, as before)
                f.Root ??= Root;
                try { show?.Invoke(FirstMessage(f)); } catch { }
                _ = Task.Run(async () =>
                {
                    var r = await SetupReport.SendAsync(f, Log);
                    if (show == null) return;
                    try
                    {
                        if (ui != null && !ui.IsDisposed && ui.IsHandleCreated) ui.BeginInvoke(new Action(() => { try { show(r.Message); } catch { } }));
                    }
                    catch (InvalidOperationException) { }
                });
            }
            catch { }
        }

        /// <summary>the same, waiting for the result at most timeoutMs (a crash that ends the process)</summary>
        public static SetupReportResult? SendNow(SetupFailure f, int timeoutMs)
        {
            try
            {
                f.Root ??= Root;
                var t = Task.Run(() => SetupReport.SendAsync(f, Log));
                return t.Wait(timeoutMs) ? t.Result : null;
            }
            catch { return null; }
        }

        // ------------------------------------------------------------------ the launcher's own crashes
        static int s_uiErrors, s_inside, s_fatal;

        static string CrashLog
        {
            get
            {
                string? root = Root;
                return root != null ? Path.Combine(root, "build", "launcher_crash.log") : Path.Combine(AppContext.BaseDirectory, "launcher_crash.log");
            }
        }

        /// <summary>a cancelled wait, no internet, a closed socket: written, not a crash, never reported</summary>
        internal static bool Expected(Exception ex)
        {
            var b = ex is AggregateException a && a.InnerExceptions.Count == 1 ? a.InnerExceptions[0] : ex;
            return b is OperationCanceledException || b is HttpRequestException || b is SocketException || b is ObjectDisposedException
                   || b is System.Net.WebException;
        }

        public static void InstallCrashHandlers()
        {
            if (!SetupReport.SwitchOn()) return;
            Application.SetUnhandledExceptionMode(UnhandledExceptionMode.CatchException);
            Application.ThreadException += (_, e) =>
            {
                if (Interlocked.Increment(ref s_uiErrors) > 20) { Write(e.Exception, "window thread, 20th error: the launcher closes"); Application.Exit(); return; }
                Handle(e.Exception, "window thread", true);
            };
            AppDomain.CurrentDomain.UnhandledException += (_, e) =>
            {
                if (Interlocked.Exchange(ref s_fatal, 1) == 1) return;
                var ex = e.ExceptionObject as Exception ?? new Exception(Convert.ToString(e.ExceptionObject));
                Write(ex, "background thread" + (e.IsTerminating ? ", the launcher closes" : ""));
                try
                {
                    var r = SendNow(Failure(ex, "background thread"), 8000);
                    MessageBox.Show("OpenSmash's launcher had an error and has to close.\n\n" + SetupReport.FirstLine(ex.GetBaseException().Message) + "\n\n" +
                                    (r != null ? r.Message + "\n\n" : "") + "Details: " + CrashLog, "OpenSmash", MessageBoxButtons.OK, MessageBoxIcon.Error);
                }
                catch { }
            };
            TaskScheduler.UnobservedTaskException += (_, e) =>
            {
                try { e.SetObserved(); } catch { }
                var ex = e.Exception?.InnerExceptions.Count == 1 ? e.Exception.InnerExceptions[0] : e.Exception;
                if (ex != null) Handle(ex, "background task", false);
            };
            string? test = Environment.GetEnvironmentVariable("OPENSMASH_THROW_TEST");
            if (!string.IsNullOrEmpty(test))
            {
                SynchronizationContext? ui = null;
                void Idle(object? s, EventArgs e) { Application.Idle -= Idle; ui = SynchronizationContext.Current; }
                Application.Idle += Idle;
                _ = Task.Run(async () =>
                {
                    await Task.Delay(6000);
                    if (test == "thread") new Thread(() => throw new InvalidOperationException("OPENSMASH_THROW_TEST: an error on a background thread")) { IsBackground = true }.Start();
                    else if (test == "task") { _ = Task.Run(new Action(() => throw new InvalidOperationException("OPENSMASH_THROW_TEST: an error in a task nobody waits for"))); await Task.Delay(1000); GC.Collect(); GC.WaitForPendingFinalizers(); }
                    else ui?.Post(_ => throw new InvalidOperationException("OPENSMASH_THROW_TEST: an error on the window's thread"), null);
                });
            }
        }

        static SetupFailure Failure(Exception ex, string thread) => new SetupFailure
        {
            Kind = "launcher_crash", Root = Root,
            Where = thread + (Where != null ? ", " + Where : ""),
            Error = SetupReport.Describe(ex.GetBaseException()),
            Detail = ex.ToString(),
            Logs = new[] { CrashLog },
        };

        static void Handle(Exception ex, string thread, bool window)
        {
            if (Interlocked.Exchange(ref s_inside, 1) == 1) return;   // (an error inside this handler: given up)
            try
            {
                if (Expected(ex))
                {
                    Write(ex, thread + ", expected (cancelled or no connection): not reported");
                    if (window) MessageBox.Show("OpenSmash could not finish: " + SetupReport.FirstLine(ex.GetBaseException().Message), "OpenSmash", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    return;
                }
                Write(ex, thread);
                var f = Failure(ex, thread);
                if (!window) { Send(f, null, null); return; }
                string first = FirstMessage(f);
                Send(f, null, null);
                MessageBox.Show("OpenSmash's launcher had an error:\n\n" + SetupReport.FirstLine(ex.GetBaseException().Message) + "\n\n" + first +
                                "\n\nDetails: " + CrashLog, "OpenSmash", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
            catch { }
            finally { Interlocked.Exchange(ref s_inside, 0); }
        }

        /// <summary>appends the error to launcher_crash.log (kept under 512 KB: the older half goes)</summary>
        static void Write(Exception? ex, string thread)
        {
            try
            {
                string path = CrashLog;
                var sb = new StringBuilder();
                sb.Append("=== ").Append(DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss")).Append("  launcher ").Append(Updater.Current)
                  .Append("  ").Append(thread).Append("  where: ").Append(Where ?? "-").Append("\r\n");
                sb.Append(ex?.ToString() ?? "(no exception)").Append("\r\n\r\n");
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                if (File.Exists(path) && new FileInfo(path).Length > 512 << 10)
                {
                    string old = File.ReadAllText(path);
                    File.WriteAllText(path, old.Substring(old.Length / 2));
                }
                File.AppendAllText(path, sb.ToString());
                Log("launcher error (" + thread + "): " + SetupReport.FirstLine(ex?.Message) + ", see launcher_crash.log");
            }
            catch { }
        }
    }
}
