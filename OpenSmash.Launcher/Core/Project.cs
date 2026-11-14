namespace OpenSmash.Core;

public enum DesktopPlatform { Windows, MacOS, Linux }

public sealed class Project
{
    public string Root { get; }
    public DesktopPlatform Platform { get; }
    public static DesktopPlatform CurrentPlatform => OperatingSystem.IsWindows() ? DesktopPlatform.Windows
        : OperatingSystem.IsMacOS() ? DesktopPlatform.MacOS : DesktopPlatform.Linux;

    public Project(string root, DesktopPlatform platform)
    {
        Root = Path.GetFullPath(root);
        Platform = platform;
        if (!IsProject(Root)) throw new ArgumentException("Choose the OpenSmash project folder containing tools and runtime.");
    }

    public static bool IsProject(string path) => Directory.Exists(Path.Combine(path, "runtime"))
        && File.Exists(Path.Combine(path, "tools", "os_mods.py"));

    public static string? AtLauncherLocation(string directory)
    {
        var location=new DirectoryInfo(Path.GetFullPath(directory));
        // macOS distributes one .app item, whose executable lives two levels inside it.
        if(location.Name=="MacOS"&&location.Parent?.Name=="Contents"&&location.Parent.Parent is {} app&&app.Name.EndsWith(".app",StringComparison.OrdinalIgnoreCase))
            location=app.Parent!;
        return IsProject(location.FullName)?location.FullName:null;
    }

    public string PathIn(params string[] parts) => Path.Combine(new[] { Root }.Concat(parts).ToArray());
    public string GameExecutable => Platform switch
    {
        DesktopPlatform.Windows => PathIn("build", "ssbu.exe"),
        DesktopPlatform.MacOS => PathIn("build-mac", "WindowSmash.app", "Contents", "MacOS", "ssbu"),
        _ => PathIn("build-linux", "ssbu")
    };
    public bool IsBuilt => File.Exists(GameExecutable);

    // Mirrors runtime/config_folder.h, including the lobby file's migration exception.
    public string ConfigFile(string name)
    {
        if (!ConfigNames.Contains(name)) throw new ArgumentException("Unknown configuration file.");
        string migrated = PathIn("config", name);
        return File.Exists(PathIn("config", "version.ini"))
            && (name != "online_server.txt" || File.Exists(migrated)) ? migrated : PathIn(name);
    }

    public static readonly string[] ConfigNames =
        ["display.ini", "controllers.ini", "online_name.txt", "online_server.txt", "online_direct.txt", "launcher.ini"];

    public string Python
    {
        get
        {
            string ready=PathIn("deps","venv",".opensmash-ready"),own=PathIn("deps","venv","Scripts","python.exe");
            if(Platform==DesktopPlatform.Windows&&File.Exists(ready)&&File.Exists(own))return own;
            string[] candidates = Platform == DesktopPlatform.Windows
                ? [PathIn("deps", "python", "python.exe"), PathIn("deps", "venv", "Scripts", "python.exe"), PathIn(".venv", "Scripts", "python.exe")]
                : [PathIn("deps", "venv", "bin", "python3"), PathIn(".venv", "bin", "python3"), PathIn("deps", "python", "bin", "python3")];
            string configured = PathIn("build", "python_path.txt");
            if (File.Exists(configured)) candidates = candidates.Prepend(File.ReadAllText(configured).Trim().Trim('"')).ToArray();
            return candidates.FirstOrDefault(File.Exists) ?? (Platform == DesktopPlatform.Windows ? "python" : "python3");
        }
    }
}
