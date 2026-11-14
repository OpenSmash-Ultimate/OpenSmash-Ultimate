// Portable drawing primitives for the original launcher's immediate-mode artwork.
// Coordinates, paths, colours and typography come from OpenSmash.Launcher; Skia
// replaces GDI+ so the same artwork can be drawn on every supported desktop.
using System.Drawing;
using SkiaSharp;
#pragma warning disable CS0618 // Compatibility facade preserves the original Skia text measurements.
#pragma warning disable CA1416 // Optional GDI objects are created/called only on Windows.

namespace OpenSmash.Visuals;

enum StringAlignment { Near, Center, Far }
[Flags] enum StringFormatFlags { NoWrap = 1, LineLimit = 2, MeasureTrailingSpaces = 4 }
enum StringTrimming { EllipsisCharacter, EllipsisWord, EllipsisPath }
enum FontStyle { Regular, Bold }
enum GraphicsUnit { Pixel }
enum CombineMode { Intersect }
enum LinearGradientMode { Horizontal, Vertical }
enum LineJoin { Miter }
sealed class StringFormat : IDisposable
{
    public StringAlignment Alignment, LineAlignment;
    public StringTrimming Trimming;
    public StringFormatFlags FormatFlags;
    public bool Typographic;
    public static StringFormat GenericTypographic => new() {Typographic=true};
    public object Clone() => MemberwiseClone();
    public void Dispose() { }
}
sealed class FontFamily(SKTypeface face)
{
    public SKTypeface Face { get; } = face;
}
sealed class Font(FontFamily family, float size, FontStyle style = FontStyle.Regular, GraphicsUnit unit = GraphicsUnit.Pixel) : IDisposable
{
    System.Drawing.Font? _windows;
    internal System.Drawing.Font Windows=>_windows??=new System.Drawing.Font(WindowsRasterizer.Family(Name),Size,(System.Drawing.FontStyle)(int)Style,System.Drawing.GraphicsUnit.Pixel);
    public Font(string family, float size, FontStyle style = FontStyle.Regular, GraphicsUnit unit = GraphicsUnit.Pixel)
        : this(new FontFamily(SKTypeface.FromFamilyName(family, style == FontStyle.Bold ? SKFontStyle.Bold : SKFontStyle.Normal)), size, style, unit) { }
    public FontFamily FontFamily { get; } = family;
    public string Name => FontFamily.Face.FamilyName;
    public float Size { get; } = size;
    public FontStyle Style { get; } = style;
    public GraphicsUnit Unit { get; } = unit;
    public SKPaint Paint(Color color) => new() { Typeface = FontFamily.Face, TextSize = Size,
        IsAntialias = true, SubpixelText = true, Color = Graphics.C(color), IsStroke = false,
        FakeBoldText=Style==FontStyle.Bold&&FontFamily.Face.FontStyle.Weight<600 };
    public void Dispose() {_windows?.Dispose();_windows=null;}
}
abstract class Brush : IDisposable
{
    internal abstract SKPaint Paint();
    public virtual void Dispose() { }
}
sealed class SolidBrush(Color color) : Brush
{
    public Color Color { get; } = color;
    internal override SKPaint Paint() => new() { Color = Graphics.C(Color), IsAntialias = true };
}
sealed class ColorBlend(int count)
{
    public Color[] Colors = new Color[count];
    public float[] Positions = new float[count];
}
sealed class LinearGradientBrush(RectangleF rect, Color from, Color to, LinearGradientMode mode) : Brush
{
    public System.Drawing.Brush WindowsBrush()
    {
        var b=new System.Drawing.Drawing2D.LinearGradientBrush(rect,from,to,mode==LinearGradientMode.Horizontal?System.Drawing.Drawing2D.LinearGradientMode.Horizontal:System.Drawing.Drawing2D.LinearGradientMode.Vertical);
        if(InterpolationColors!=null)b.InterpolationColors=new System.Drawing.Drawing2D.ColorBlend(InterpolationColors.Colors.Length){Colors=InterpolationColors.Colors,Positions=InterpolationColors.Positions};return b;
    }
    public ColorBlend? InterpolationColors;
    internal override SKPaint Paint() => new() { IsAntialias = true,
        Shader = SKShader.CreateLinearGradient(new(rect.Left, rect.Top),
            mode == LinearGradientMode.Horizontal ? new(rect.Right, rect.Top) : new(rect.Left, rect.Bottom),
            InterpolationColors?.Colors.Select(Graphics.C).ToArray() ?? [Graphics.C(from), Graphics.C(to)],
            InterpolationColors?.Positions ?? [0, 1], SKShaderTileMode.Clamp) };
}
sealed class PathGradientBrush(GraphicsPath path) : Brush
{
    public System.Drawing.Brush WindowsBrush()=>new System.Drawing.Drawing2D.PathGradientBrush(path.Windows!){CenterColor=CenterColor,SurroundColors=SurroundColors};
    public Color CenterColor;
    public Color[] SurroundColors = [Color.Transparent];
    internal override SKPaint Paint()
    {
        var r = path.Path.Bounds;
        return new SKPaint { IsAntialias=true, Shader=SKShader.CreateRadialGradient(
            new(r.MidX,r.MidY), r.Width/2, [Graphics.C(CenterColor),Graphics.C(SurroundColors[0])],
            [0,1],SKShaderTileMode.Clamp, SKMatrix.CreateScale(1,r.Height/r.Width,r.MidX,r.MidY)) };
    }
}
sealed class Pen(Color color, float width = 1) : IDisposable
{
    public Color Color=>color;
    public float Width=>width;
    public LineJoin LineJoin;
    internal SKPaint Paint() => new() { Color = Graphics.C(color), StrokeWidth = width,
        Style = SKPaintStyle.Stroke, StrokeJoin = SKStrokeJoin.Miter, IsAntialias = true };
    public void Dispose() { }
}
sealed class GraphicsPath : IDisposable
{
    public System.Drawing.Drawing2D.GraphicsPath? Windows { get; }=WindowsRasterizer.Enabled?new():null;
    public SKPath Path { get; } = new();
    public void AddRectangle(RectangleF r) {Path.AddRect(Graphics.R(r));Windows?.AddRectangle(r);}
    public void AddEllipse(RectangleF r) {Path.AddOval(Graphics.R(r));Windows?.AddEllipse(r);}
    public void AddPolygon(PointF[] points)
    {
        Path.MoveTo(points[0].X, points[0].Y);
        foreach (var point in points.Skip(1)) Path.LineTo(point.X, point.Y);
        Path.Close();
        Windows?.AddPolygon(points);
    }
    public void AddArc(float x, float y, float w, float h, float start, float sweep) {Path.ArcTo(new(x,y,x+w,y+h), start, sweep, false);Windows?.AddArc(x,y,w,h,start,sweep);}
    public void AddString(string text, FontFamily family, int style, float size, PointF point, StringFormat format)
    {
        if(Windows!=null){using var f=WindowsRasterizer.Format(format);Windows.AddString(text,WindowsRasterizer.Family(family.Face.FamilyName),style,size,point,f);return;}
        using var paint = new Font(family, size).Paint(Color.White);
        using var textPath = paint.GetTextPath(text, point.X, point.Y - paint.FontMetrics.Ascent);
        Path.AddPath(textPath);
    }
    public void CloseFigure() {Path.Close();Windows?.CloseFigure();}
    public RectangleF GetBounds() {if(Windows!=null)return Windows.GetBounds();var r = Path.TightBounds; return new(r.Left, r.Top, r.Width, r.Height); }
    public void Dispose() {Path.Dispose();Windows?.Dispose();}
}
sealed class GraphicsState(int count,System.Drawing.Drawing2D.GraphicsState? windows=null) { public int Count { get; } = count;public System.Drawing.Drawing2D.GraphicsState? Windows=>windows; }
sealed class Matrix(float a,float b,float c,float d,float x,float y)
{
    public System.Drawing.Drawing2D.Matrix Windows=>new(a,b,c,d,x,y);
    public SKMatrix Native => new() { ScaleX=a,SkewY=b,SkewX=c,ScaleY=d,TransX=x,TransY=y,Persp2=1 };
}
sealed class Bitmap : IDisposable
{
    internal System.Drawing.Bitmap? Windows;
    SKBitmap? _scaled;
    public byte[]? Encoded { get; }
    public SKBitmap Native { get; }
    public int Width => Native.Width;
    public int Height => Native.Height;
    public Bitmap(int w, int h) { Native = new(w,h, SKColorType.Bgra8888, SKAlphaType.Premul); Native.Erase(SKColors.Transparent); }
    public Bitmap(SKBitmap bitmap) { Native = bitmap; }
    public Bitmap(byte[] encoded) {Encoded=encoded;Native=SKBitmap.Decode(encoded);}
    // Skia: a large image drawn small (the 512px emblem in a 34px header) is resampled once per size, not every frame.
    internal SKBitmap Scaled(int w,int h)
    {
        if(_scaled==null||_scaled.Width!=w||_scaled.Height!=h){_scaled?.Dispose();_scaled=Native.Resize(new SKImageInfo(w,h,Native.ColorType,Native.AlphaType),new SKSamplingOptions(SKCubicResampler.Mitchell));}
        return _scaled??Native;
    }
    internal static bool Shrinks(Bitmap image,Rectangle dest)=>dest.Width>0&&dest.Height>0&&dest.Width*2<=image.Width&&dest.Height*2<=image.Height;
    public void Dispose() {Windows?.Dispose();_scaled?.Dispose();Native.Dispose();}
}
sealed class Graphics(SKCanvas canvas, bool owns = false,System.Drawing.Graphics? windows=null,System.Drawing.Bitmap? windowsFrame=null) : IDisposable
{
    bool _gridFit;
    public bool GridFit {get=>_gridFit;set{_gridFit=value;if(windows!=null)windows.TextRenderingHint=value?System.Drawing.Text.TextRenderingHint.AntiAliasGridFit:System.Drawing.Text.TextRenderingHint.AntiAlias;}}
    public static Graphics ForFrame(SKCanvas canvas,int w,int h,IntPtr pixels,int stride)
    {
        if(!WindowsRasterizer.Enabled)return new(canvas);
        var bitmap=new System.Drawing.Bitmap(w,h,stride,System.Drawing.Imaging.PixelFormat.Format32bppPArgb,pixels);
        return new(canvas,false,WindowsRasterizer.CreateGraphics(bitmap,false),bitmap);
    }
    public SKCanvas Canvas => canvas;
    public static SKColor C(Color c) => new(c.R,c.G,c.B,c.A);
    public static SKRect R(RectangleF r) => new(r.Left,r.Top,r.Right,r.Bottom);
    public static Graphics FromImage(Bitmap image)
    {
        if(!WindowsRasterizer.Enabled)return new(new SKCanvas(image.Native),true);
        var bitmap=WindowsRasterizer.View(image);return new(new SKCanvas(image.Native),true,WindowsRasterizer.CreateGraphics(bitmap,false));
    }
    public GraphicsState Save() => new(canvas.Save(),windows?.Save());
    public void Restore(GraphicsState state) {canvas.RestoreToCount(state.Count);if(state.Windows!=null)windows!.Restore(state.Windows);}
    public void TranslateTransform(float x, float y) {canvas.Translate(x,y);windows?.TranslateTransform(x,y);}
    public void RotateTransform(float degrees) {canvas.RotateDegrees(degrees);windows?.RotateTransform(degrees);}
    public void ScaleTransform(float x, float y) {canvas.Scale(x,y);windows?.ScaleTransform(x,y);}
    public void MultiplyTransform(Matrix matrix) { var m=matrix.Native; canvas.Concat(in m);if(windows!=null){using var w=matrix.Windows;windows.MultiplyTransform(w);} }
    public void SetClip(RectangleF r, CombineMode mode) {canvas.ClipRect(R(r));windows?.SetClip(r,System.Drawing.Drawing2D.CombineMode.Intersect);}
    public void SetClip(RectangleF r) {canvas.ClipRect(R(r));windows?.SetClip(r);}
    public void SetClip(GraphicsPath path, CombineMode mode) {canvas.ClipPath(path.Path);windows?.SetClip(path.Windows!,System.Drawing.Drawing2D.CombineMode.Intersect);}
    public void FillPath(Brush b, GraphicsPath path) {if(windows!=null){using var wb=WindowsRasterizer.Brush(b);windows.FillPath(wb,path.Windows!);return;}using var p = b.Paint(); canvas.DrawPath(path.Path,p); }
    public void DrawPath(Pen pen, GraphicsPath path) {if(windows!=null){using var wp=WindowsRasterizer.Pen(pen);windows.DrawPath(wp,path.Windows!);return;}using var p = pen.Paint(); canvas.DrawPath(path.Path,p); }
    public void FillRectangle(Brush b, float x, float y, float w, float h) {if(windows!=null){using var wb=WindowsRasterizer.Brush(b);windows.FillRectangle(wb,x,y,w,h);return;}using var p = b.Paint(); canvas.DrawRect(x,y,Math.Max(0,w),Math.Max(0,h),p); }
    public void FillRectangle(Brush b, RectangleF r) => FillRectangle(b,r.X,r.Y,r.Width,r.Height);
    public void DrawRectangle(Pen pen,float x,float y,float w,float h)
    {if(windows!=null){using var wp=WindowsRasterizer.Pen(pen);windows.DrawRectangle(wp,x,y,w,h);return;}using var p=pen.Paint();canvas.DrawRect(x,y,w,h,p);}
    public void FillPolygon(Brush b, PointF[] points) { using var p = new GraphicsPath(); p.AddPolygon(points); FillPath(b,p); }
    public void DrawLine(Pen pen, float x, float y, float x2, float y2) {if(windows!=null){using var wp=WindowsRasterizer.Pen(pen);windows.DrawLine(wp,x,y,x2,y2);return;}using var p = pen.Paint(); canvas.DrawLine(x,y,x2,y2,p); }
    public void DrawLines(Pen pen, PointF[] points) {if(windows!=null){using var wp=WindowsRasterizer.Pen(pen);windows.DrawLines(wp,points);return;}for(int i=1;i<points.Length;i++) DrawLine(pen,points[i-1].X,points[i-1].Y,points[i].X,points[i].Y); }
    public void FillEllipse(Brush b,float x,float y,float w,float h) {if(windows!=null){using var wb=WindowsRasterizer.Brush(b);windows.FillEllipse(wb,x,y,w,h);return;}using var p = b.Paint(); canvas.DrawOval(new SKRect(x,y,x+w,y+h),p); }
    public void FillEllipse(Brush b,RectangleF r) => FillEllipse(b,r.X,r.Y,r.Width,r.Height);
    public void DrawEllipse(Pen b,float x,float y,float w,float h) {if(windows!=null){using var wp=WindowsRasterizer.Pen(b);windows.DrawEllipse(wp,x,y,w,h);return;}using var p = b.Paint(); canvas.DrawOval(new SKRect(x,y,x+w,y+h),p); }
    public void DrawImageUnscaled(Bitmap image,int x,int y) {if(windows!=null){var view=WindowsRasterizer.View(image);windows.DrawImageUnscaled(view,x,y);return;}canvas.DrawBitmap(image.Native,x,y);}
    // An opaque full-frame layer: copy it instead of blending it over the previous frame.
    public void CopyImage(Bitmap image)
    {
        if(windows!=null){var mode=windows.CompositingMode;windows.CompositingMode=System.Drawing.Drawing2D.CompositingMode.SourceCopy;windows.DrawImageUnscaled(WindowsRasterizer.View(image),0,0);windows.CompositingMode=mode;return;}
        using var p=new SKPaint{BlendMode=SKBlendMode.Src};canvas.DrawBitmap(image.Native,0,0,p);
    }
    public void DrawImage(Bitmap image, RectangleF r, float alpha=1)
    {
        var dest=Rectangle.Round(r);
        if(windows!=null)
        {
            var view=WindowsRasterizer.View(image);
            if(alpha>=1){windows.DrawImage(view,dest,0,0,view.Width,view.Height,System.Drawing.GraphicsUnit.Pixel);return;}
            using var ia=new System.Drawing.Imaging.ImageAttributes();ia.SetColorMatrix(new System.Drawing.Imaging.ColorMatrix{Matrix33=alpha});windows.DrawImage(view,dest,0,0,view.Width,view.Height,System.Drawing.GraphicsUnit.Pixel,ia);return;
        }
        using var p = new SKPaint { Color = SKColors.White.WithAlpha((byte)(255*alpha)), FilterQuality=SKFilterQuality.High }; canvas.DrawBitmap(Bitmap.Shrinks(image,dest)?image.Scaled(dest.Width,dest.Height):image.Native,R(r),p);
    }
    public void DrawImage(Bitmap image,float x,float y,float w,float h)=>DrawImage(image,new RectangleF(x,y,w,h));
    public SizeF MeasureString(string text, Font font, int max=100000, StringFormat? format=null)
    {
        if(windows!=null){var wf=WindowsRasterizer.Font(font);if(format==null)return windows.MeasureString(text,wf,max);using var sf=WindowsRasterizer.Format(format);return windows.MeasureString(text,wf,max,sf);}
        using var p=font.Paint(Color.White);p.SubpixelText=!GridFit;
        float pad=format?.Typographic==true?0:font.Size/3,lineH=p.FontSpacing+(format?.Typographic==true?0:font.Size/8);
        var lines=max<100000&&!((format?.FormatFlags.HasFlag(StringFormatFlags.NoWrap))??false)?Wrap(text,p,Math.Max(1,max-pad)):text.Split('\n').ToList();
        return new(lines.Max(p.MeasureText)+pad,lineH*lines.Count);
    }
    static List<string> Wrap(string text,SKPaint paint,float width)
    {
        var lines=new List<string>();
        foreach(string paragraph in text.Replace("\r","").Split('\n'))
        {
            string line="";foreach(string word in paragraph.Split(' '))
            {string next=line.Length==0?word:line+" "+word;if(paint.MeasureText(next)>width&&line.Length>0){lines.Add(line);line=word;}else line=next;}
            lines.Add(line);
        }
        return lines;
    }
    public void DrawString(string text,Font font,Brush brush,float x,float y,StringFormat format)
        => DrawString(text,font,brush,new RectangleF(x,y,100000,100000),format,true);
    public void DrawString(string text,Font font,Brush brush,RectangleF rect,StringFormat format)
        => DrawString(text,font,brush,rect,format,false);
    void DrawString(string text,Font font,Brush brush,RectangleF rect,StringFormat format,bool point)
    {
        if(windows!=null)
        {
            var f=WindowsRasterizer.Font(font);using var b=WindowsRasterizer.Brush(brush);using var sf=WindowsRasterizer.Format(format);
            if(point)windows.DrawString(text,f,b,rect.X,rect.Y,sf);else windows.DrawString(text,f,b,rect,sf);return;
        }
        using var p=font.Paint(brush is SolidBrush solid?solid.Color:Color.White);
        p.SubpixelText=!GridFit;
        float padding=format.Typographic?0:font.Size/6;
        if(!point)rect=new RectangleF(rect.X+padding,rect.Y,Math.Max(1,rect.Width-2*padding),rect.Height);
        float lineH=p.FontSpacing+(format.Typographic?0:font.Size/8);
        var lines=new List<string>();
        if(!format.FormatFlags.HasFlag(StringFormatFlags.NoWrap) && !point)
        {
            lines=Wrap(text,p,rect.Width);
        }
        else lines.Add(text);
        int maxLines=point?lines.Count:Math.Max(1,(int)(rect.Height/lineH));
        if(lines.Count>maxLines) lines=lines.Take(maxLines).ToList();
        float top=point?rect.Y:format.LineAlignment switch { StringAlignment.Center=>rect.Y+(rect.Height-lines.Count*lineH)/2, StringAlignment.Far=>rect.Bottom-lines.Count*lineH, _=>rect.Y };
        int save=canvas.Save();
        if(!point) canvas.ClipRect(R(rect));
        foreach(string raw in lines)
        {
            string line=raw;
            if(!point && p.MeasureText(line)>rect.Width)
            {
                if(format.Trimming==StringTrimming.EllipsisPath)
                {
                    int n=line.Length/2;
                    while(n>0 && p.MeasureText(line[..n]+"…"+line[^n..])>rect.Width) n--;
                    line=n==0?"…":line[..n]+"…"+line[^n..];
                }
                else { while(line.Length>0 && p.MeasureText(line+"…")>rect.Width) line=line[..^1]; line+="…"; }
            }
            float x=rect.X+(format.Alignment==StringAlignment.Center?(rect.Width-p.MeasureText(line))/2:format.Alignment==StringAlignment.Far?rect.Width-p.MeasureText(line):0);
            canvas.DrawText(line,x,top-p.FontMetrics.Ascent,p); top+=lineH;
        }
        canvas.RestoreToCount(save);
    }
    public void Dispose() {windows?.Dispose();windowsFrame?.Dispose();if(owns) canvas.Dispose(); }
}
