using Avalonia.Controls;
using SkiaSharp;

namespace OpenSmash.Visuals;

public sealed partial class ClassicSurface
{
    WindowIcon? _themeWindowIcon;
    byte[] MakeThemeIcon()
    {
        if (_logo == null) return [];
        var images = new List<(int Size, byte[] Data)>();
        foreach (int size in new[] { 16, 32, 48, 64, 128, 256 })
        {
            using var bitmap = _logo.Native.Resize(new SKImageInfo(size, size), new SKSamplingOptions(SKCubicResampler.Mitchell));
            using var image = SKImage.FromBitmap(bitmap);
            using var png = image.Encode(SKEncodedImageFormat.Png, 100);
            images.Add((size, png.ToArray()));
        }
        using var output = new MemoryStream();
        using var writer = new BinaryWriter(output);
        writer.Write((ushort)0); writer.Write((ushort)1); writer.Write((ushort)images.Count);
        int offset = 6 + images.Count * 16;
        foreach (var (size, data) in images)
        {
            writer.Write((byte)(size == 256 ? 0 : size)); writer.Write((byte)(size == 256 ? 0 : size));
            writer.Write((byte)0); writer.Write((byte)0); writer.Write((ushort)1); writer.Write((ushort)32);
            writer.Write(data.Length); writer.Write(offset); offset += data.Length;
        }
        foreach (var (_, data) in images) writer.Write(data);
        return output.ToArray();
    }

    void ApplyWindowIcon()
    {
        if (_logo == null || TopLevel.GetTopLevel(this) is not Window window) return;
        if (_themeWindowIcon == null)
        {
            using var stream = new MemoryStream(MakeThemeIcon());
            _themeWindowIcon = new WindowIcon(stream);
        }
        window.Icon = _themeWindowIcon;
        foreach (var owned in window.OwnedWindows) owned.Icon = _themeWindowIcon;
    }
}
