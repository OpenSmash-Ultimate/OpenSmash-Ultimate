using System.Reflection;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;
using Avalonia.Threading;
using OpenSmash;
using OpenSmash.Visuals;

static class EyedropperChecks
{
    static void Check(bool ok, string message) { if (!ok) throw new Exception(message); Console.WriteLine("PASS " + message); }
    static T Field<T>(Window picker, string name) => (T)picker.GetType().GetField(name, BindingFlags.Instance|BindingFlags.NonPublic)!.GetValue(picker)!;
    static Task Pick(Window picker) => (Task)picker.GetType().GetMethod("PickScreenColor",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(picker,null)!;
    static Window Make(Func<Window,Task<Color?>> sample, bool supported = true)
    {
        var type = typeof(ClassicSurface).Assembly.GetType("OpenSmash.Visuals.ThemeColorPicker")!;
        return (Window)Activator.CreateInstance(type, BindingFlags.Instance|BindingFlags.NonPublic, null,
            new object[] { "Accent color", "#FF3B30", sample, supported }, null)!;
    }

    public static void Run(MainWindow owner)
    {
        var selected = Color.FromRgb(0x12,0xAB,0xEF);
        using (var picker = new WindowScope(Make(_ => Task.FromResult<Color?>(selected))))
        {
            picker.Value.Show(owner);
            Pick(picker.Value).GetAwaiter().GetResult();
            Check(Field<TextBox>(picker.Value,"_hex").Text=="#12ABEF", "eyedropper result updates editable hex");
            var wheel=Field<Control>(picker.Value,"_wheel");
            Check((Color)wheel.GetType().GetProperty("SelectedColor")!.GetValue(wheel)! == selected, "eyedropper result synchronizes the wheel");
            Check(picker.Value.Opacity==1 && Field<Button>(picker.Value,"_eyedropper").IsEnabled, "eyedropper restores the visible, usable picker");
        }
        using (var picker = new WindowScope(Make(_ => Task.FromResult<Color?>(null))))
        {
            picker.Value.Show(owner); Pick(picker.Value).GetAwaiter().GetResult();
            Check(Field<TextBox>(picker.Value,"_hex").Text=="#FF3B30", "canceling screen selection preserves the original color");
        }
        using (var picker = new WindowScope(Make(_ => Task.FromException<Color?>(new System.ComponentModel.Win32Exception("Capture unavailable")))))
        {
            picker.Value.Show(owner); Pick(picker.Value).GetAwaiter().GetResult();
            Check(Field<TextBlock>(picker.Value,"_error").Text=="Capture unavailable" && picker.Value.Opacity==1,
                "capture errors restore the picker and explain the failure");
        }
        var completion = new TaskCompletionSource<Color?>();
        int calls=0;
        using (var picker = new WindowScope(Make(_ => { calls++; return completion.Task; })))
        {
            picker.Value.Show(owner);
            var pending=Pick(picker.Value); Pick(picker.Value).GetAwaiter().GetResult();
            Check(calls==1 && picker.Value.Opacity==0, "screen selection hides the picker and cannot start twice");
            picker.Value.Close(); completion.SetResult(selected);
            Pump(pending);
            Check(!picker.Value.IsVisible, "finishing capture cannot reopen a closed picker");
        }
        using (var picker = new WindowScope(Make(_ => Task.FromResult<Color?>(null),supported:false)))
            Check(!Field<Button>(picker.Value,"_eyedropper").IsVisible, "unsupported platforms do not offer a nonworking eyedropper");
        var service=typeof(ClassicSurface).Assembly.GetType("OpenSmash.Visuals.ScreenEyedropper")!;
        var decoded=(Color)service.GetMethod("FromColorRef",BindingFlags.Static|BindingFlags.NonPublic)!.Invoke(null,new object[]{0x00EFAB12u})!;
        Check(decoded==selected, "Windows COLORREF converts to RGB without swapping channels");
        if (OperatingSystem.IsWindows())
        {
            using var picker = new WindowScope(Make(_ => Task.FromResult<Color?>(null)));
            picker.Value.Show(owner);
            var task=(Task<Color?>)service.GetMethod("PickAsync")!.Invoke(null,new object[]{picker.Value})!;
            var overlays=picker.Value.OwnedWindows.ToArray();
            Check(overlays.Length==picker.Value.Screens.All.Count && overlays.All(window=>window.Topmost&&!window.ShowInTaskbar),
                "screen selection covers every monitor without adding taskbar buttons");
            overlays[0].RaiseEvent(new KeyEventArgs { RoutedEvent=InputElement.KeyDownEvent, Key=Key.Escape });
            Pump(task);
            Check(task.Result==null && overlays.All(window=>!window.IsVisible), "Escape cancels and removes every screen overlay");
        }
    }

    static void Pump(Task task)
    {
        if (!SpinWait.SpinUntil(()=>{Dispatcher.UIThread.RunJobs();return task.IsCompleted;},3000)) throw new TimeoutException("Screen selection did not finish.");
        task.GetAwaiter().GetResult();
    }
    sealed class WindowScope(Window value) : IDisposable
    {
        public Window Value { get; } = value;
        public void Dispose() => Value.Close();
    }
}
