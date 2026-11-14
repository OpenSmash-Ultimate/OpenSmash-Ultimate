using System.Reflection;
using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Threading;
using OpenSmash;

static class NativeWindowChecks
{
    [DllImport("user32.dll")] static extern int GetWindowLong(IntPtr window, int index);
    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)] static extern int SetCurrentProcessExplicitAppUserModelID(string id);

    public static void Run(bool taskbar = false)
    {
        if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException("This check needs a real Windows desktop.");
        Exception? failure = null;
        var thread = new Thread(() => { try { CheckNativeWindow(taskbar); } catch (Exception error) { failure = error; } });
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start(); thread.Join();
        if (failure != null) throw new InvalidOperationException("Native Windows minimize/restore check failed.",failure);
    }

    static System.Diagnostics.Process ClickTaskbar()
    {
        const string script = """
            $ErrorActionPreference = 'Stop'
            Add-Type -AssemblyName UIAutomationClient
            Add-Type -AssemblyName UIAutomationTypes
            $root = [System.Windows.Automation.AutomationElement]::RootElement
            $bar = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ClassNameProperty, 'Shell_TrayWnd'))
            if ($null -eq $bar) { throw 'No Windows taskbar accessible' }
            $buttons = $bar.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Button))
            $matches = @($buttons | Where-Object { $_.Current.AutomationId -eq 'Appid: OpenSmash.RestoreRegressionTest' })
            if ($matches.Count -ne 1) { throw ('Expected one test taskbar button; found ' + $matches.Count) }
            Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public static class TaskbarInput { [DllImport("user32.dll")] public static extern bool SetPhysicalCursorPos(int x, int y); [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra); }'
            $rect = $matches[0].Current.BoundingRectangle
            if ($rect.Width -le 0 -or $rect.Height -le 0 -or $matches[0].Current.IsOffscreen) { throw 'Test taskbar button is not visible' }
            if (-not [TaskbarInput]::SetPhysicalCursorPos([int]($rect.X+$rect.Width/2), [int]($rect.Y+$rect.Height/2))) { throw 'Could not target taskbar button' }
            [TaskbarInput]::mouse_event(2,0,0,0,[UIntPtr]::Zero)
            [TaskbarInput]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
            """;
        var start = new System.Diagnostics.ProcessStartInfo("powershell.exe") { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (string arg in new[] { "-NoProfile", "-Command", script }) start.ArgumentList.Add(arg);
        return System.Diagnostics.Process.Start(start)!;
    }

    static void CheckNativeWindow(bool taskbar)
    {
        string root = Path.Combine(Path.GetTempPath(),"OpenSmash native window test " + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Path.Combine(root,"runtime"));
        Directory.CreateDirectory(Path.Combine(root,"tools"));
        File.WriteAllText(Path.Combine(root,"tools","os_mods.py"),"");
        MainWindow? window = null;
        try
        {
            Marshal.ThrowExceptionForHR(SetCurrentProcessExplicitAppUserModelID("OpenSmash.RestoreRegressionTest"));
            AppBuilder.Configure<App>().UsePlatformDetect().WithInterFont().SetupWithoutStarting();
            window = new MainWindow([],root,enableBackgroundActions:false);
            window.Title = "OpenSmash restore regression";
            window.CanResize = false;
            window.Show();
            IntPtr handle = window.TryGetPlatformHandle()!.Handle;
            uint style = unchecked((uint)GetWindowLong(handle,-16));
            if ((style & 0x00CA0000) != 0x00CA0000) throw new Exception("Missing native caption, system menu or minimize style.");
            Console.WriteLine("PASS real Windows HWND retains native shell minimize/restore styles");
            if (window.WindowDecorations != WindowDecorations.Full || !window.ExtendClientAreaToDecorationsHint || window.ExtendClientAreaTitleBarHeightHint != 0) throw new Exception("Custom title bar must use native extended-client-area shell handling.");
            Console.WriteLine("PASS launcher keeps its custom borderless title bar");
            using var stop = new CancellationTokenSource();
            var clock = System.Diagnostics.Stopwatch.StartNew();
            int stage = 0, cycles = 0;
            System.Diagnostics.Process? click = null;
            Exception? error = null;
            var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(100) };
            timer.Tick += (_, _) =>
            {
                try
                {
                    if (clock.Elapsed > TimeSpan.FromSeconds(45)) throw new TimeoutException("Minimize/restore did not complete.");
                    if (stage == 0)
                    {
                        typeof(MainWindow).GetMethod("MinimizeWindow",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(window,null);
                        stage = 1;
                    }
                    else if (stage == 1 && IsIconic(handle) && window.WindowState == WindowState.Minimized)
                    {
                        if (taskbar) click = ClickTaskbar();
                        else if (!PostMessage(handle,0x0112,new IntPtr(0xF120),IntPtr.Zero)) throw new Exception("Could not send shell restore.");
                        stage = 2;
                    }
                    else if (stage == 2 && (click == null || click.HasExited))
                    {
                        if (click != null)
                        {
                            string output = click.StandardOutput.ReadToEnd(), errors = click.StandardError.ReadToEnd();
                            int code = click.ExitCode; click.Dispose(); click = null;
                            if (code != 0) throw new Exception("Taskbar click failed: " + errors + output);
                        }
                        stage = 3;
                    }
                    else if (stage == 3 && !IsIconic(handle) && window.WindowState == WindowState.Normal)
                    {
                        Console.WriteLine("PASS native minimize and " + (taskbar ? "single taskbar click" : "single-command shell restore") + ", cycle " + ++cycles);
                        if (cycles == 3) { timer.Stop(); stop.Cancel(); }
                        else stage = 0;
                    }
                }
                catch (Exception failure) { error = failure; timer.Stop(); stop.Cancel(); }
            };
            timer.Start();
            Dispatcher.UIThread.MainLoop(stop.Token);
            if (click != null) { if (!click.HasExited) click.Kill(); click.Dispose(); }
            if (error != null) throw error;
        }
        finally { window?.Close(); Directory.Delete(root,true); }
    }
}
