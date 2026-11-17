using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Chrome;
using Avalonia.Styling;

namespace OpenSmash;

public sealed partial class MainWindow
{
    void InitializeWindowsChrome()
    {
        if (!OperatingSystem.IsWindows()) return;
        Win32Properties.AddWndProcHookCallback(this, RestoreBeforeActivation);
        // Keep Windows' shell state handling, but draw our own header over the extended client area.
        WindowDecorations = WindowDecorations.Full;
        ExtendClientAreaToDecorationsHint = true;
        ExtendClientAreaTitleBarHeightHint = 0;
        WindowDecorationsTheme = new ControlTheme(typeof(WindowDrawnDecorations))
        {
            Setters =
            {
                new Setter(WindowDrawnDecorations.TemplateProperty, new EmptyChromeTemplate()),
                new Setter(WindowDrawnDecorations.DefaultTitleBarHeightProperty, 0d),
                new Setter(WindowDrawnDecorations.DefaultFrameThicknessProperty, new Thickness(0)),
                new Setter(WindowDrawnDecorations.DefaultShadowThicknessProperty, new Thickness(0))
            }
        };
    }

    sealed class EmptyChromeTemplate : IWindowDrawnDecorationsTemplate
    {
        public Avalonia.Controls.Templates.TemplateResult<WindowDrawnDecorationsContent> Build() =>
            new(new WindowDrawnDecorationsContent { Overlay = new Panel { Height = 0, IsHitTestVisible = false } }, new NameScope());
        object? ITemplate.Build() => Build().Result;
    }

    IntPtr RestoreBeforeActivation(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        // Restore before Avalonia moves focus into a window Windows still considers iconic.
        if (message == 0x0006 && (wParam.ToInt64() & 0xFFFF) != 0 && IsIconic(hwnd) && IsWindowEnabled(hwnd))
            ShowWindow(hwnd, 9);
        return IntPtr.Zero;
    }

    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] static extern bool IsIconic(IntPtr hwnd);
    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] static extern bool IsWindowEnabled(IntPtr hwnd);
    [DllImport("user32.dll")] [return: MarshalAs(UnmanagedType.Bool)] static extern bool ShowWindow(IntPtr hwnd, int command);

    void MinimizeWindow()
    {
        if (OperatingSystem.IsWindows() && TryGetPlatformHandle() is { HandleDescriptor: "HWND" } handle
            && PostMessage(handle.Handle, 0x0112, new IntPtr(0xF020), IntPtr.Zero)) return;
        WindowState = WindowState.Minimized;
    }

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
}
