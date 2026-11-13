using System.Diagnostics;

namespace OpenSmash.Core;

public sealed record Command(string Executable, IReadOnlyList<string> Arguments, string WorkingDirectory)
{
    public ProcessStartInfo StartInfo(bool capture = false)
    {
        var info = new ProcessStartInfo(Executable) { WorkingDirectory = WorkingDirectory,
            UseShellExecute = false, CreateNoWindow = capture,
            RedirectStandardOutput = capture, RedirectStandardError = capture,
            RedirectStandardInput = capture };
        foreach (string argument in Arguments) info.ArgumentList.Add(argument);
        info.Environment["PYTHONIOENCODING"] = "utf-8";
        info.Environment["PYTHONUNBUFFERED"] = "1";
        return info;
    }
}

public static class Commands
{
    public sealed record InstallOptions(string Game,string Keys,string Firmware,string Name,int Jobs,
        bool Shortcuts=true,bool Firewall=true,bool Shaders=true,bool Updates=true,bool AcceptLicense=true);

    public static Command Install(Project p,InstallOptions options)
    {
        if(p.Platform==DesktopPlatform.Windows)
        {
            var args=new List<string>{"-NoProfile","-ExecutionPolicy","Bypass","-File",p.PathIn("tools","install_windowsmash.ps1"),
                "-NoWizard","-Game",options.Game,"-Keys",options.Keys,"-OnlineName",options.Name,"-Jobs",options.Jobs.ToString()};
            if(options.Firmware=="")args.Add("-NoFirmware");else args.AddRange(["-Firmware",options.Firmware]);
            if(options.Shortcuts)args.Add("-Shortcut");if(options.Firewall)args.Add("-Firewall");if(options.Shaders)args.Add("-Shaders");
            if(!options.Updates)args.Add("-NoUpdate");if(options.AcceptLicense)args.Add("-AcceptLicense");
            return new("powershell.exe",args,p.Root);
        }
        var unix=new List<string>{p.Platform==DesktopPlatform.MacOS?p.PathIn("tools","mac","install.sh"):p.PathIn("tools","linux","install.sh"),
            "--yes","--game",options.Game,"--keys",options.Keys,"--online-name",options.Name,"--jobs",options.Jobs.ToString()};
        if(p.Platform==DesktopPlatform.MacOS)unix.Add("--no-wizard");
        if(options.Firmware=="")unix.Add("--no-firmware");else unix.AddRange(["--firmware",options.Firmware]);
        if(!options.Updates)unix.Add("--no-update");
        return new("/bin/bash",unix,p.Root);
    }
    public static readonly string[] ClearedGameVariables = ["SSBU_NET_BACKEND", "SSBU_DATA_ROOT", "SSBU_INPUT", "SSBU_RENDERER",
        "SSBU_NET_HOST", "SSBU_NET_RELAY", "SSBU_NET_BIND", "SSBU_NET_ROOM", "SSBU_NET_NAME", "SSBU_NET_DIRECT", "SSBU_NET_PORT"];

    public static Command Play(Project project, bool online) => project.Platform switch
    {
        DesktopPlatform.Windows => new(project.GameExecutable,
            online ? ["dump/modules", "--status", "30", "--online"] : ["dump/modules", "--status", "30"], project.Root),
        DesktopPlatform.MacOS => new("/bin/bash", [project.PathIn("tools", "mac", "launch.sh"), "--gui", online ? "play" : "offline"], project.Root),
        _ => new("/bin/bash", [online ? project.PathIn("tools","linux","play.sh") : project.PathIn("launchers", "play-ssbu-offline.sh")], project.Root)
    };

    public static Command Mods(Project p) => new(p.Python,
        [p.PathIn("tools", "modmanager", "server.py"), "--tree", p.Root], p.Root);

    public static void OpenPath(string path) => Process.Start(new ProcessStartInfo(path) { UseShellExecute = true })?.Dispose();
}
