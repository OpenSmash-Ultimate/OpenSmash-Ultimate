using System.ComponentModel;
using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;

namespace OpenSmash.Visuals;

static class ScreenEyedropper
{
    public static async Task<Color?> PickAsync(Window owner)
    {
        if (!OperatingSystem.IsWindows()) throw new PlatformNotSupportedException("Screen color picking is currently available on Windows.");
        var choice = new TaskCompletionSource<PixelPoint?>(TaskCreationOptions.RunContinuationsAsynchronously);
        var overlays = new List<Window>();
        void OwnerClosed(object? sender, EventArgs args) => choice.TrySetResult(null);
        owner.Closed += OwnerClosed;
        PixelPoint? selected;
        try
        {
            foreach (var screen in owner.Screens.All)
            {
                var overlay = new Window
                {
                    Title = "Pick a screen color — click or press Enter; Escape cancels",
                    WindowDecorations = WindowDecorations.None, ShowInTaskbar = false, CanResize = false,
                    Topmost = true, WindowStartupLocation = WindowStartupLocation.Manual,
                    Position = screen.Bounds.Position,
                    Width = screen.Bounds.Width / screen.Scaling, Height = screen.Bounds.Height / screen.Scaling,
                    TransparencyLevelHint = [WindowTransparencyLevel.Transparent],
                    Background = Avalonia.Media.Brush.Parse("#01000000"), Cursor = new Cursor(StandardCursorType.Cross),
                    Content = new Border
                    {
                        HorizontalAlignment = Avalonia.Layout.HorizontalAlignment.Left,
                        VerticalAlignment = Avalonia.Layout.VerticalAlignment.Top,
                        Margin = new Thickness(20), Padding = new Thickness(12), Background = Brushes.Black,
                        Child = new TextBlock { Text = "Click a color anywhere · Enter picks at cursor · Esc cancels", Foreground = Brushes.White }
                    }
                };
                overlays.Add(overlay);
                bool pressed = false, cancelPressed = false;
                overlay.PointerPressed += (_, e) =>
                {
                    var properties = e.GetCurrentPoint(overlay).Properties;
                    if (properties.IsRightButtonPressed) { cancelPressed = true; e.Pointer.Capture(overlay); }
                    else if (properties.IsLeftButtonPressed) { pressed = true; e.Pointer.Capture(overlay); }
                    e.Handled = true;
                };
                overlay.PointerReleased += (_, e) =>
                {
                    if (cancelPressed) choice.TrySetResult(null);
                    else if (pressed) choice.TrySetResult(overlay.PointToScreen(e.GetPosition(overlay)));
                    pressed = cancelPressed = false; e.Pointer.Capture(null); e.Handled = true;
                };
                overlay.PointerCaptureLost += (_, _) => pressed = cancelPressed = false;
                overlay.KeyDown += (_, e) =>
                {
                    if (e.Key == Key.Escape) { choice.TrySetResult(null); e.Handled = true; }
                    else if (e.Key == Key.Enter)
                    {
                        if (GetPhysicalCursorPos(out var point)) choice.TrySetResult(new PixelPoint(point.X, point.Y));
                        else choice.TrySetException(new Win32Exception(Marshal.GetLastWin32Error()));
                        e.Handled = true;
                    }
                };
                overlay.Closing += (_, _) => choice.TrySetResult(null);
                overlay.Show(owner);
                overlay.Position = screen.Bounds.Position;
                overlay.Width = screen.Bounds.Width / overlay.RenderScaling;
                overlay.Height = screen.Bounds.Height / overlay.RenderScaling;
            }
            if (overlays.Count == 0) throw new InvalidOperationException("No desktop screens are available.");
            selected = await choice.Task;
        }
        finally
        {
            owner.Closed -= OwnerClosed;
            foreach (var overlay in overlays) overlay.Close();
        }
        if (selected is not { } pixel) return null;
        // Remove the translucent selection windows from the compositor before reading the real pixel.
        DwmFlush();
        return ReadPixel(pixel);
    }

    internal static Color FromColorRef(uint pixel) => Color.FromRgb((byte)pixel, (byte)(pixel >> 8), (byte)(pixel >> 16));

    static Color ReadPixel(PixelPoint point)
    {
        IntPtr desktop = GetDC(IntPtr.Zero);
        if (desktop == IntPtr.Zero) throw new Win32Exception("Windows could not access the desktop color.");
        try
        {
            uint pixel = GetPixel(desktop, point.X, point.Y);
            if (pixel == uint.MaxValue) throw new Win32Exception("Windows could not read that screen pixel. Try a visible app on your desktop.");
            return FromColorRef(pixel);
        }
        finally { ReleaseDC(IntPtr.Zero, desktop); }
    }

    [StructLayout(LayoutKind.Sequential)] struct NativePoint { public int X, Y; }
    [DllImport("user32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] static extern bool GetPhysicalCursorPos(out NativePoint point);
    [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr hwnd);
    [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr hwnd, IntPtr dc);
    [DllImport("gdi32.dll")] static extern uint GetPixel(IntPtr dc, int x, int y);
    [DllImport("dwmapi.dll")] static extern int DwmFlush();
}
