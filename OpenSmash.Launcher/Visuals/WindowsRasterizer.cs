// GDI+ is used only on Windows, where it preserves the original raster output.
// The portable surface and all drawing methods remain shared with Skia elsewhere.
#pragma warning disable CA1416 // Every entry point is guarded by OperatingSystem.IsWindows().
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.Runtime.InteropServices;
using Avalonia.Platform;

namespace OpenSmash.Visuals;

static class WindowsRasterizer
{
    internal static bool Enabled=>OperatingSystem.IsWindows()&&!string.Equals(Environment.GetEnvironmentVariable("OPENSMASH_RENDERER"),"skia",StringComparison.OrdinalIgnoreCase);
    static readonly Lazy<PrivateFontCollection> Fonts=new(LoadFonts);
    static readonly Dictionary<string,System.Drawing.FontFamily> Families=new();
    static PrivateFontCollection LoadFonts()
    {
        var fonts=new PrivateFontCollection();
        foreach(var uri in new[]{"avares://OpenSmash/Assets/Anton-Regular.ttf","avares://OpenSmash/Assets/Inter-Regular.ttf","avares://OpenSmash/Assets/Inter-Bold.ttf","avares://OpenSmash/Assets/CascadiaMono.ttf"})
        {
            using var source=AssetLoader.Open(new Uri(uri));using var memory=new MemoryStream();source.CopyTo(memory);byte[] bytes=memory.ToArray();
            // GDI+ retains the font memory for the lifetime of the process.
            IntPtr pointer=Marshal.AllocCoTaskMem(bytes.Length);Marshal.Copy(bytes,0,pointer,bytes.Length);fonts.AddMemoryFont(pointer,bytes.Length);
        }
        return fonts;
    }
    public static System.Drawing.FontFamily Family(string name)
    {
        if(!Families.TryGetValue(name,out var family))Families[name]=family=Fonts.Value.Families.FirstOrDefault(f=>f.Name==name)??new System.Drawing.FontFamily(name);
        return family;
    }
    public static int TextBoxHeight(float pixels)
    {using var font=new System.Drawing.Font("Segoe UI",pixels,System.Drawing.FontStyle.Regular,System.Drawing.GraphicsUnit.Pixel);return font.Height+7;}
    public static System.Drawing.Font Font(Font font)=>font.Windows;
    public static System.Drawing.StringFormat Format(StringFormat format)
    {
        var result=format.Typographic?(System.Drawing.StringFormat)System.Drawing.StringFormat.GenericTypographic.Clone():new System.Drawing.StringFormat();
        result.Alignment=(System.Drawing.StringAlignment)(int)format.Alignment;result.LineAlignment=(System.Drawing.StringAlignment)(int)format.LineAlignment;
        result.Trimming=format.Trimming switch {StringTrimming.EllipsisPath=>System.Drawing.StringTrimming.EllipsisPath,StringTrimming.EllipsisWord=>System.Drawing.StringTrimming.EllipsisWord,_=>System.Drawing.StringTrimming.EllipsisCharacter};
        if(format.FormatFlags.HasFlag(StringFormatFlags.NoWrap))result.FormatFlags|=System.Drawing.StringFormatFlags.NoWrap;
        if(format.FormatFlags.HasFlag(StringFormatFlags.LineLimit))result.FormatFlags|=System.Drawing.StringFormatFlags.LineLimit;
        if(format.FormatFlags.HasFlag(StringFormatFlags.MeasureTrailingSpaces))result.FormatFlags|=System.Drawing.StringFormatFlags.MeasureTrailingSpaces;
        return result;
    }
    public static System.Drawing.Brush Brush(Brush brush)=>brush switch
    {
        SolidBrush solid=>new System.Drawing.SolidBrush(solid.Color),
        LinearGradientBrush linear=>linear.WindowsBrush(),
        PathGradientBrush radial=>radial.WindowsBrush(),
        _=>throw new NotSupportedException(brush.GetType().Name)
    };
    public static System.Drawing.Pen Pen(Pen pen)=>new(pen.Color,pen.Width){LineJoin=System.Drawing.Drawing2D.LineJoin.Miter};
    public static System.Drawing.Bitmap View(Bitmap bitmap)
    {
        if(bitmap.Windows==null)
        {
            if(bitmap.Encoded!=null){using var stream=new MemoryStream(bitmap.Encoded);using var original=new System.Drawing.Bitmap(stream);bitmap.Windows=(System.Drawing.Bitmap)original.Clone();}
            else bitmap.Windows=new(bitmap.Width,bitmap.Height,bitmap.Native.RowBytes,System.Drawing.Imaging.PixelFormat.Format32bppPArgb,bitmap.Native.GetPixels());
        }
        return bitmap.Windows;
    }
    public static System.Drawing.Graphics CreateGraphics(System.Drawing.Bitmap bitmap,bool gridFit)
    {
        var g=System.Drawing.Graphics.FromImage(bitmap);g.SmoothingMode=SmoothingMode.AntiAlias;g.PixelOffsetMode=PixelOffsetMode.HighQuality;
        g.InterpolationMode=InterpolationMode.HighQualityBicubic;g.TextRenderingHint=gridFit?TextRenderingHint.AntiAliasGridFit:TextRenderingHint.AntiAlias;
        return g;
    }
}
