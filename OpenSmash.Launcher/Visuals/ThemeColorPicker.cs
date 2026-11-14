using System.Globalization;
using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;

namespace OpenSmash.Visuals;

sealed class ThemeColorPicker : Window
{
    readonly HueWheel _wheel = new();
    readonly Slider _brightness = new() { Minimum = 0, Maximum = 1, Value = 1, SmallChange = .01, LargeChange = .1 };
    readonly TextBox _hex = new() { MaxLength = 7, PlaceholderText = "#RRGGBB" };
    readonly Border _preview = new() { Height = 36, CornerRadius = new CornerRadius(4) };
    readonly TextBlock _error = new() { Foreground = Brushes.Salmon, TextWrapping = TextWrapping.Wrap, MinHeight = 22 };
    readonly Button _apply = new() { Content = "Apply", IsDefault = true };
    readonly Button _eyedropper = new() { Content = "Eyedropper � pick from screen" };
    readonly Func<Window, Task<Color?>> _sampleScreen;
    bool _syncing, _sampling, _closed;

    public ThemeColorPicker(string title, string initial)
        : this(title, initial, ScreenEyedropper.PickAsync, OperatingSystem.IsWindows()) { }

    internal ThemeColorPicker(string title, string initial, Func<Window, Task<Color?>> sampleScreen, bool supportsScreenPicking)
    {
        _sampleScreen = sampleScreen;
        Title = title;
        Width = 380; SizeToContent = SizeToContent.Height; CanResize = false;
        Opened += (_, _) => { if (Screens.ScreenFromWindow(this) is { } screen) MaxHeight = screen.WorkingArea.Height / screen.Scaling * .9; };
        Closed += (_, _) => _closed = true;
        _eyedropper.IsVisible = supportsScreenPicking;
        AutomationProperties.SetName(_eyedropper, "Eyedropper: pick a color from another app");
        ToolTip.SetTip(_eyedropper, "Click a color anywhere on your desktop. Press Escape to cancel.");
        _eyedropper.Click += async (_, _) => await PickScreenColor();
        WindowStartupLocation = WindowStartupLocation.CenterOwner;
        Background = Avalonia.Media.Brush.Parse("#151517"); Foreground = Brushes.White;
        AutomationProperties.SetName(_wheel, "Color wheel. Arrow keys change hue and saturation.");
        AutomationProperties.SetName(_brightness, "Brightness");
        AutomationProperties.SetName(_hex, "Hex color, #RRGGBB");
        var cancel = new Button { Content = "Cancel", IsCancel = true };
        var buttons = new StackPanel { Orientation = Avalonia.Layout.Orientation.Horizontal, Spacing = 12,
            HorizontalAlignment = Avalonia.Layout.HorizontalAlignment.Right, Children = { cancel, _apply } };
        Content = new ScrollViewer { VerticalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Auto, Content = new StackPanel { Margin = new Thickness(24), Spacing = 10, Children =
        {
            new TextBlock { Text = title, FontSize = 22 },
            new TextBlock { Text = "Pick a color, or enter a hex code.", Foreground = Brushes.LightGray },
            _wheel, _eyedropper, new TextBlock { Text = "Brightness" }, _brightness,
            new TextBlock { Text = "Hex color" }, _hex, _preview, _error, buttons
        }}};
        _wheel.Changed += RefreshColor;
        _brightness.PropertyChanged += (_, e) =>
        {
            if (e.Property == Slider.ValueProperty && !_syncing) { _wheel.Brightness = _brightness.Value; RefreshColor(); }
        };
        _hex.PropertyChanged += (_, e) =>
        {
            if (e.Property != TextBox.TextProperty || _syncing) return;
            bool valid = TryHex(_hex.Text ?? "", out var color);
            _apply.IsEnabled = valid;
            _error.Text = valid ? "" : "Use # followed by six hex digits (0–9, A–F).";
            if (!valid) return;
            _syncing = true;
            _wheel.SetColor(color);
            _brightness.Value = _wheel.Brightness;
            _preview.Background = new SolidColorBrush(color);
            _syncing = false;
        };
        _apply.Click += (_, _) => { if (TryHex(_hex.Text ?? "", out var color)) Close(Format(color)); };
        cancel.Click += (_, _) => Close(null);
        _hex.Text = initial;
    }

    async Task PickScreenColor()
    {
        if (_sampling || _closed) return;
        _sampling = true; _eyedropper.IsEnabled = false;
        double opacity = Opacity;
        try
        {
            Opacity = 0;
            var color = await _sampleScreen(this);
            if (!_closed && color is { } selected) _hex.Text = Format(selected);
        }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException or PlatformNotSupportedException)
        { if (!_closed) _error.Text = error.Message; }
        finally
        {
            Opacity = opacity;
            _sampling = false; _eyedropper.IsEnabled = true;
            if (!_closed) { Activate(); _hex.Focus(); }
        }
    }

    void RefreshColor()
    {
        if (_syncing) return;
        _syncing = true;
        var color = _wheel.SelectedColor;
        _hex.Text = Format(color);
        _preview.Background = new SolidColorBrush(color);
        _apply.IsEnabled = true; _error.Text = "";
        _syncing = false;
    }

    internal static bool TryHex(string text, out Color color)
    {
        color = default;
        if (text.Length != 7 || text[0] != '#' || !uint.TryParse(text.AsSpan(1),
            NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out uint rgb)) return false;
        color = Color.FromRgb((byte)(rgb >> 16), (byte)(rgb >> 8), (byte)rgb);
        return true;
    }

    internal static string Format(Color color) => $"#{color.R:X2}{color.G:X2}{color.B:X2}";
}

sealed class HueWheel : Control
{
    const int Diameter = 224;
    WriteableBitmap? _image;
    double _hue, _saturation, _brightness = 1;
    bool _dragging;
    public event Action? Changed;

    public HueWheel()
    {
        Width = Height = Diameter;
        Focusable = true;
        HorizontalAlignment = Avalonia.Layout.HorizontalAlignment.Center;
    }

    public double Brightness
    {
        get => _brightness;
        set { _brightness = Math.Clamp(value, 0, 1); InvalidateVisual(); }
    }
    public Color SelectedColor => FromHsv(_hue, _saturation, _brightness);

    public void SetColor(Color color)
    {
        double max = Math.Max(color.R, Math.Max(color.G, color.B));
        double min = Math.Min(color.R, Math.Min(color.G, color.B));
        double delta = max - min;
        _brightness = max / 255;
        _saturation = max == 0 ? 0 : delta / max;
        if (delta != 0)
        {
            _hue = max == color.R ? 60 * ((color.G - color.B) / delta % 6)
                : max == color.G ? 60 * ((color.B - color.R) / delta + 2)
                : 60 * ((color.R - color.G) / delta + 4);
            _hue = (_hue + 360) % 360;
        }
        InvalidateVisual();
    }

    internal static Color FromHsv(double hue, double saturation, double value)
    {
        double c = value * saturation, x = c * (1 - Math.Abs(hue / 60 % 2 - 1)), m = value - c;
        var (r,g,b) = hue switch
        {
            < 60 => (c,x,0d), < 120 => (x,c,0d), < 180 => (0d,c,x),
            < 240 => (0d,x,c), < 300 => (x,0d,c), _ => (c,0d,x)
        };
        return Color.FromRgb((byte)Math.Round((r+m)*255), (byte)Math.Round((g+m)*255), (byte)Math.Round((b+m)*255));
    }

    public override void Render(DrawingContext context)
    {
        base.Render(context);
        if (_image == null)
        {
            _image = new WriteableBitmap(new PixelSize(Diameter,Diameter), new Vector(96,96), PixelFormat.Bgra8888, AlphaFormat.Premul);
            using var buffer = _image.Lock();
            var pixels = new byte[buffer.RowBytes * Diameter];
            for (int y = 0; y < Diameter; y++)
                for (int x = 0; x < Diameter; x++)
                {
                    double dx = (x+.5-Diameter/2d)/(Diameter/2d), dy = (y+.5-Diameter/2d)/(Diameter/2d);
                    double saturation = Math.Sqrt(dx*dx+dy*dy);
                    if (saturation > 1) continue;
                    var color = FromHsv((Math.Atan2(dy,dx)*180/Math.PI+360)%360,saturation,1);
                    int i = y*buffer.RowBytes+x*4;
                    pixels[i]=color.B; pixels[i+1]=color.G; pixels[i+2]=color.R; pixels[i+3]=255;
                }
            Marshal.Copy(pixels,0,buffer.Address,pixels.Length);
        }
        context.DrawImage(_image,new Rect(0,0,Diameter,Diameter));
        double angle = _hue*Math.PI/180;
        var center = new Point(Diameter/2d+Math.Cos(angle)*_saturation*Diameter/2d,
            Diameter/2d+Math.Sin(angle)*_saturation*Diameter/2d);
        context.DrawEllipse(null,new Avalonia.Media.Pen(Brushes.Black,4),center,6,6);
        context.DrawEllipse(null,new Avalonia.Media.Pen(IsFocused ? Brushes.White : Brushes.LightGray,2),center,6,6);
    }

    void Pick(Point point)
    {
        double dx = point.X-Diameter/2d, dy = point.Y-Diameter/2d;
        _hue = (Math.Atan2(dy,dx)*180/Math.PI+360)%360;
        _saturation = Math.Min(1,Math.Sqrt(dx*dx+dy*dy)/(Diameter/2d));
        InvalidateVisual(); Changed?.Invoke();
    }
    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        base.OnPointerPressed(e);
        if (!e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) return;
        var point = e.GetPosition(this);
        if (Math.Pow(point.X-Diameter/2d,2)+Math.Pow(point.Y-Diameter/2d,2)>Diameter*Diameter/4d) return;
        Focus(); _dragging=true; e.Pointer.Capture(this); Pick(point); e.Handled=true;
    }
    protected override void OnPointerMoved(PointerEventArgs e)
    { base.OnPointerMoved(e); if (_dragging) Pick(e.GetPosition(this)); }
    protected override void OnPointerReleased(PointerReleasedEventArgs e)
    { base.OnPointerReleased(e); _dragging=false; e.Pointer.Capture(null); }
    protected override void OnPointerCaptureLost(PointerCaptureLostEventArgs e)
    { base.OnPointerCaptureLost(e); _dragging=false; }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        base.OnKeyDown(e);
        if (e.Key is not (Key.Left or Key.Right or Key.Up or Key.Down)) return;
        if (e.Key is Key.Left or Key.Right) _hue=(_hue+(e.Key==Key.Left?355:5))%360;
        else _saturation=Math.Clamp(_saturation+(e.Key==Key.Up?.025:-.025),0,1);
        InvalidateVisual(); Changed?.Invoke(); e.Handled=true;
    }
    protected override void OnDetachedFromVisualTree(VisualTreeAttachmentEventArgs e)
    { base.OnDetachedFromVisualTree(e); _image?.Dispose(); _image=null; }
}
