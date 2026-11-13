namespace OpenSmash.Core;

// Older pinned source archives use root-level shell entry points. Keep those
// archives installable while giving every platform the same internal layout.
public static class ProjectLayout
{
    static readonly (string Old,string Current,string Resource)[] Scripts =
    [
        ("install-windowsmash.sh","tools/linux/install.sh","layout.linux.install.sh"),
        ("Install WindowSmash.command","tools/mac/install.sh","layout.mac.install.sh"),
        ("play-ssbu.sh","tools/linux/play.sh","layout.linux.play.sh"),
        ("Play SSBU.command","tools/mac/play.sh","layout.mac.play.sh")
    ];
    public static void Normalize(string root)
    {
        bool legacy=Scripts.Any(s=>File.Exists(Path.Combine(root,s.Old)));
        if(!legacy)return;
        foreach(var script in Scripts)
        {
            string target=Path.Combine(root,script.Current);
            if(File.Exists(target))continue;
            using var resource=typeof(ProjectLayout).Assembly.GetManifestResourceStream(script.Resource)
                ??throw new InvalidOperationException("Missing internal setup script: "+script.Current);
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            using(var output=File.Create(target))resource.CopyTo(output);
            if(!OperatingSystem.IsWindows())File.SetUnixFileMode(target,UnixFileMode.UserRead|UnixFileMode.UserWrite|UnixFileMode.UserExecute|UnixFileMode.GroupRead|UnixFileMode.GroupExecute|UnixFileMode.OtherRead|UnixFileMode.OtherExecute);
        }
        var callers=new List<string>{"tools/linux/install_linux.py","tools/mac/install_mac.py","tools/mac/launch.sh"};
        string launchers=Path.Combine(root,"launchers");
        if(Directory.Exists(launchers))callers.AddRange(Directory.EnumerateFiles(launchers).Where(p=>p.EndsWith(".sh")||p.EndsWith(".command")).Select(p=>Path.GetRelativePath(root,p)));
        foreach(string caller in callers)
        {
            string path=Path.Combine(root,caller);if(!File.Exists(path))continue;
            string before=File.ReadAllText(path),after=before;
            foreach(var script in Scripts)after=after.Replace(script.Old,script.Current,StringComparison.Ordinal);
            if(after!=before)File.WriteAllText(path,after);
        }
        // Remove old entry points only after the replacement scripts and callers exist.
        foreach(string old in Scripts.Select(s=>s.Old).Concat(["Install WindowSmash.bat","Play SSBU.bat"]))
        {string path=Path.Combine(root,old);if(File.Exists(path))File.Delete(path);}
    }
}
