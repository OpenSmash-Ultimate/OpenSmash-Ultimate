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
        string askpass=Path.Combine(root,"tools","linux","askpass.sh");
        if(!File.Exists(askpass))
        {
            Directory.CreateDirectory(Path.GetDirectoryName(askpass)!);
            File.WriteAllText(askpass,Resource("layout.linux.askpass.sh"));
            if(!OperatingSystem.IsWindows())File.SetUnixFileMode(askpass,UnixFileMode.UserRead|UnixFileMode.UserWrite|UnixFileMode.UserExecute|UnixFileMode.GroupRead|UnixFileMode.GroupExecute|UnixFileMode.OtherRead|UnixFileMode.OtherExecute);
        }
        var callers=new List<string>{"tools/linux/install_linux.py","tools/mac/install_mac.py","tools/mac/launch.sh"};
        string launchers=Path.Combine(root,"launchers");
        if(Directory.Exists(launchers))callers.AddRange(Directory.EnumerateFiles(launchers).Where(p=>p.EndsWith(".sh")||p.EndsWith(".command")).Select(p=>Path.GetRelativePath(root,p)));
        foreach(string caller in callers)
        {
            string path=Path.Combine(root,caller);if(!File.Exists(path))continue;
            string before=File.ReadAllText(path),after=before;
            foreach(var script in Scripts)after=after.Replace(script.Old,script.Current,StringComparison.Ordinal);
            // Only layout/authorization callbacks are adapted. The downloaded
            // backend's game, dependency and update implementation stays intact.
            if(caller=="tools/linux/install_linux.py")
            {
                string template=Resource("layout.linux.backend.py");
                foreach(string name in new[]{"desktop_entry","sudo_command","sudo_run","step_launchers"})after=ReplaceFunction(after,template,name);
                after=after.Replace("\"opensmash-launcher.sh\", ","");
            }
            else if(caller=="tools/mac/install_mac.py")
            {
                after=ReplaceFunction(after,Resource("layout.mac.backend.py"),"step_launchers");
                after=after.Replace(", \"OpenSmash Launcher.command\"","");
            }
            if(after!=before)File.WriteAllText(path,after);
        }
        // Remove old entry points only after the replacement scripts and callers exist.
        foreach(string old in Scripts.Select(s=>s.Old).Concat(["Install WindowSmash.bat","Play SSBU.bat","opensmash-launcher.sh","OpenSmash Launcher.command","OpenSmash Launcher.desktop"]))
        {string path=Path.Combine(root,old);if(File.Exists(path))File.Delete(path);}
    }
    static string Resource(string name)
    {
        using var stream=typeof(ProjectLayout).Assembly.GetManifestResourceStream(name)??throw new InvalidOperationException("Missing layout resource: "+name);
        using var reader=new StreamReader(stream);return reader.ReadToEnd();
    }
    static string ReplaceFunction(string code,string template,string name)
    {
        string signature="def "+name+"(";int from=template.IndexOf(signature,StringComparison.Ordinal);
        if(from<0)throw new InvalidOperationException("Missing layout callback: "+name);
        int end=template.IndexOf("\ndef ",from+signature.Length,StringComparison.Ordinal);if(end<0)end=template.Length;
        string replacement=template[from..end].TrimEnd()+"\n\n";
        int at=code.IndexOf(signature,StringComparison.Ordinal);
        if(at<0)
        {
            // Older Linux backends have no sudo_command; place it before its caller.
            if(name!="sudo_command")return code;
            at=code.IndexOf("def sudo_run(",StringComparison.Ordinal);return at<0?code:code.Insert(at,replacement);
        }
        int next=code.IndexOf("\ndef ",at+signature.Length,StringComparison.Ordinal);if(next<0)next=code.Length;
        return code[..at]+replacement+code[next..];
    }
}
