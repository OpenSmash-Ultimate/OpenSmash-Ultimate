using System.IO.Compression;
using System.Security.Cryptography;

namespace OpenSmash.Core;

public static class LegalNotices
{
    public static void Open()
    {
        using var archive = typeof(LegalNotices).Assembly.GetManifestResourceStream("ThirdPartyNotices.zip");
        if(archive == null) { Commands.OpenPath(AppContext.BaseDirectory); return; }
        using var bytes = new MemoryStream(); archive.CopyTo(bytes);
        string identity = Convert.ToHexString(SHA256.HashData(bytes.ToArray()))[..16];
        string target = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "OpenSmash", "licenses", identity);
        Directory.CreateDirectory(target); bytes.Position = 0;
        ZipFile.ExtractToDirectory(bytes, target, overwriteFiles:true);
        Commands.OpenPath(target);
    }
}
