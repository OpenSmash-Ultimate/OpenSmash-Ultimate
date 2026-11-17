using OpenSmash.Core;
namespace OpenSmash;
public sealed partial class MainWindow
{
    // Epic Online Services' and Discord's files beside the program (tools/online_files.py: nothing downloaded when they
    // are current). An update whose fetch failed left them out and the game went online through the public lobby alone,
    // unseen by the players on Epic (issues #389, #394): every Play and every Update puts them back. Never stops either.
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
