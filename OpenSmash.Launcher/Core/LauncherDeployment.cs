namespace OpenSmash.Core;

public sealed record LauncherDeployment(string Executable, DesktopPlatform Platform)
{
    public static LauncherDeployment Current => new(Environment.ProcessPath ?? throw new IOException("Could not find this launcher."), Project.CurrentPlatform);

    public string CopyInto(string project)
    {
        if (Platform == DesktopPlatform.MacOS)
        {
            var dir = new DirectoryInfo(Path.GetDirectoryName(Executable)!);
            while (dir != null && !dir.Name.EndsWith(".app", StringComparison.OrdinalIgnoreCase)) dir = dir.Parent;
            if (dir == null) throw new IOException("Run the published OpenSmash.app to set up a new project.");
            string destination = Path.Combine(project, "OpenSmash.app");
            CopyApp(dir.FullName, destination);
            return Path.Combine(destination, "Contents", "MacOS", Path.GetFileName(Executable));
        }
        string target = Path.Combine(project, Platform == DesktopPlatform.Windows ? "OpenSmash.exe" : "OpenSmash");
        File.Copy(Executable, target, overwrite:true);
        if (!OperatingSystem.IsWindows()) File.SetUnixFileMode(target, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute | UnixFileMode.GroupRead | UnixFileMode.GroupExecute | UnixFileMode.OtherRead | UnixFileMode.OtherExecute);
        return target;
    }

    static void CopyApp(string source, string destination)
    {
        Directory.CreateDirectory(destination);
        foreach (string entry in Directory.EnumerateFileSystemEntries(source))
        {
            var info = new FileInfo(entry);
            if ((info.Attributes & FileAttributes.ReparsePoint) != 0) throw new IOException("The launcher app contains an unsupported linked file.");
            string target = Path.Combine(destination, Path.GetFileName(entry));
            if (Directory.Exists(entry)) CopyApp(entry, target);
            else
            {
                File.Copy(entry, target, overwrite:true);
                if (!OperatingSystem.IsWindows()) File.SetUnixFileMode(target, File.GetUnixFileMode(entry));
            }
        }
    }
}
