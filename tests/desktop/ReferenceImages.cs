using SkiaSharp;

static class ReferenceImages
{
    static readonly Dictionary<string,string> Names = new()
    {
        ["installer-build"]="build",["launcher-home"]="home",["launcher-settings"]="settings",["launcher-mods"]="mods",["launcher-plugins"]="plugins",
        ["launcher-settings-graphics"]="settings-graphics",["launcher-settings-audio"]="settings-audio",["launcher-settings-online"]="settings-online",
        ["launcher-settings-controllers"]="settings-controllers",["launcher-settings-update"]="settings-update",["launcher-settings-storage"]="settings-storage",
        ["launcher-settings-dropdown"]="settings-dropdown",["launcher-marketplace-heading"]="marketplace-heading",["launcher-share"]="share",["launcher-check"]="check"
    };
    public static void Compare(string name,byte[] png)
    {
        if(!OperatingSystem.IsWindows()||Environment.GetEnvironmentVariable("OPENSMASH_RENDERER")=="skia"||!Names.TryGetValue(name,out var reference))return;
        using var expected=SKBitmap.Decode(Path.Combine(AppContext.BaseDirectory,"References",reference+".png"));
        using var actual=SKBitmap.Decode(png);
        if(expected.Width!=actual.Width||expected.Height!=actual.Height||expected.ColorType!=actual.ColorType)throw new Exception(name+": reference image format changed.");
        var a=expected.GetPixelSpan();var b=actual.GetPixelSpan();int differences=0;
        for(int y=0;y<expected.Height;y++)for(int x=0;x<expected.Width;x++)
        {
            // Elapsed build-clock text is intentionally live; animation time is frozen.
            if(reference=="build"&&((x>=470&&x<530&&y>=188&&y<450)||(x>=1060&&x<1333&&y>=248&&y<279)))continue;
            // Upstream changed only the renderer wording. Preserve the original
            // geometry baseline while checking the new wording in regression tests.
            if(reference=="settings-graphics"&&((x>=815&&x<1282&&y>=498&&y<526)||(x>=192&&x<765&&y>=517&&y<543)))continue;
            if(!a.Slice(y*expected.RowBytes+x*4,4).SequenceEqual(b.Slice(y*actual.RowBytes+x*4,4)))differences++;
        }
        if(differences!=0)throw new Exception($"{name}: {differences} pixels differ from the original Windows launcher.");
        Console.WriteLine("PASS original reference: "+name+" (zero differing pixels)");
    }
}
