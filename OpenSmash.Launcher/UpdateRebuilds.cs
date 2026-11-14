using System.Diagnostics;
using OpenSmash.Core;
namespace OpenSmash;
public sealed partial class MainWindow
{
    async Task RunUpdateStep(Command command,string log)
    {
        var lines=new List<string>();int exit=await ProcessRunner.Run(command,line=>{if(lines.Count==20)lines.RemoveAt(0);lines.Add(line);},CancellationToken.None,logPath:RequireProject().PathIn("build","logs",log),environment:Commands.GuiEnvironment(RequireProject()));
        if(exit!=0)throw new InvalidOperationException("Update step stopped (exit "+exit+").\n"+string.Join("\n",lines));
    }
    bool CanBuildSharedLauncher()=>File.Exists(RequireProject().PathIn("OpenSmash.Launcher","OpenSmash.csproj"))&&File.Exists(RequireProject().PathIn("tools","build_launcher.py"));
    async Task<bool> DotnetSdkAvailable()
    {
        try{var lines=new List<string>();return await ProcessRunner.Run(new Command("dotnet",["--list-sdks"],RequireProject().Root),lines.Add,CancellationToken.None)==0&&lines.Any(l=>l.Length>0&&char.IsDigit(l[0]));}
        catch(System.ComponentModel.Win32Exception){return false;}
    }
    async Task RebuildUpdatedGame()
    {
        var p=RequireProject();string marker=UpdateState.GameMarker(p);if(!File.Exists(marker))return;
        Surface.SetOperationLabel("REBUILDING THE GAME");
        Command command=p.Platform==DesktopPlatform.Windows
            ?new("powershell.exe",["-NoProfile","-ExecutionPolicy","Bypass","-File",p.PathIn("tools","install_windowsmash.ps1"),"-BuildAfterUpdate"],p.Root)
            :new("/bin/bash",new[]{p.PathIn("tools",p.Platform==DesktopPlatform.MacOS?"mac":"linux","install.sh"),"--yes","--no-update"}.Concat(p.Platform==DesktopPlatform.MacOS?new[]{"--no-wizard"}:Array.Empty<string>()).ToArray(),p.Root);
        await RunUpdateStep(command,"update_game_build.log");
        File.Delete(marker);
    }
    async Task<bool> FinishPendingGameUpdate()
    {
        if(!File.Exists(UpdateState.GameMarker(RequireProject())))return true;
        Surface.OperationRunning=true;
        try{await EnsureToolsPython();await RebuildUpdatedGame();return true;}
        finally{Surface.OperationRunning=false;Surface.InvalidateVisual();}
    }
    bool TryResumePendingLauncherBuild()
    {
        var p=RequireProject();if(!File.Exists(UpdateState.LauncherMarker(p))||File.Exists(UpdateState.GameMarker(p))||!CanBuildSharedLauncher())return false;
        Surface.Confirm("FINISH LAUNCHER UPDATE?","The launcher update did not finish last time. Rebuild and restart it now? The .NET SDK is required.",async()=>
        {try{if(!await DotnetSdkAvailable())throw new InvalidOperationException("Install the .NET SDK or download the new launcher release.");ScheduleLauncherBuildAndRestart();}catch(Exception error){Surface.Alert("LAUNCHER UPDATE PENDING",error.Message);}});
        return true;
    }
    void ScheduleLauncherBuildAndRestart()
    {
        var p=RequireProject();if(!CanBuildSharedLauncher())throw new InvalidOperationException("The shared launcher sources are missing. Download the current launcher release.");
        UpdateState.Mark(UpdateState.LauncherMarker(p),DateTime.UtcNow.ToString("O"));
        Command command=p.Platform==DesktopPlatform.Windows
            ?new("powershell.exe",["-NoProfile","-ExecutionPolicy","Bypass","-File",p.PathIn("tools","rebuild_launcher_after_update.ps1"),"-WaitForProcessId",Environment.ProcessId.ToString(),"-Repo",p.Root,"-LauncherPath",p.PathIn("OpenSmash.exe"),"-PendingMarker",UpdateState.LauncherMarker(p)],p.Root)
            :new(p.Python,[p.PathIn("tools","rebuild_launcher_after_update.py"),"--pid",Environment.ProcessId.ToString(),"--root",p.Root],p.Root);
        var info=command.StartInfo();info.CreateNoWindow=true;
        using var process=Process.Start(info)??throw new InvalidOperationException("Could not start the launcher rebuild helper.");Close();
    }
}
