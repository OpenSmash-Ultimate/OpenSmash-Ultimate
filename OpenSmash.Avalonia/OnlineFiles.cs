using OpenSmash.Core;
namespace OpenSmash;
public sealed partial class MainWindow
{
    // Discord's files beside the program (tools/online_files.py: nothing downloaded when they are current; Epic Online
    // Services' too before 0.5.0, issues #389, #394): every Play and every Update puts them back. Never stops either.
    async Task EnsureOnlineFiles()
    {
        try
        {
            var p=RequireProject();string script=p.PathIn("tools","online_files.py");
            if(!File.Exists(script))return;
            Directory.CreateDirectory(p.PathIn("build","logs"));
            await ProcessRunner.Run(new Command(p.Python,[script,Path.GetDirectoryName(p.GameExecutable)!],p.Root),_=>{},CancellationToken.None,
                logPath:p.PathIn("build","logs","online_files.log"));
        }
        catch(Exception error) when(error is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception or InvalidOperationException){}
    }
}
