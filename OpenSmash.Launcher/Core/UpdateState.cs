using System.Security.Cryptography;
using System.Text;
namespace OpenSmash.Core;
public static class UpdateState
{
    public static string GameMarker(Project p)=>p.PathIn("config","update-game-build.pending");
    public static string LauncherMarker(Project p)=>p.PathIn("config","update-launcher-build.pending");
    public static void Mark(string path,string version){Directory.CreateDirectory(Path.GetDirectoryName(path)!);File.WriteAllText(path,version);}
    public static string LauncherFingerprint(Project p)
    {
        string folder=p.PathIn("OpenSmash.Launcher");if(!Directory.Exists(folder))throw new DirectoryNotFoundException(folder);
        var files=Directory.EnumerateFiles(folder,"*",SearchOption.AllDirectories)
            .Where(path=>!Path.GetRelativePath(folder,path).Split(Path.DirectorySeparatorChar).Any(part=>part is "bin" or "obj"))
            .Concat(new[]{"tools/build_launcher.ps1","tools/build_launcher.py","tools/build_desktop_launcher.py","tools/linux/install.sh","tools/linux/play.sh","tools/mac/install.sh","tools/mac/play.sh"}.Select(path=>p.PathIn(path)))
            .Where(File.Exists).OrderBy(path=>Path.GetRelativePath(p.Root,path),StringComparer.Ordinal);
        using var hash=IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        foreach(string path in files){hash.AppendData(Encoding.UTF8.GetBytes(Path.GetRelativePath(p.Root,path).Replace('\\','/')+"\0"));using var input=File.OpenRead(path);byte[] data=new byte[81920];int count;while((count=input.Read(data))>0)hash.AppendData(data,0,count);}
        return Convert.ToHexString(hash.GetHashAndReset());
    }
}
