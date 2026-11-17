using OpenSmash.Core;
namespace OpenSmash;
public sealed partial class MainWindow
{
    // Settings > Update > BUILD GAME + LAUNCHER (main's update:build). The game is built in place
    // (Windows: tools/rebuild-all.ps1 -GameOnly; elsewhere tools/build_game.py), then the launcher is rebuilt and
    // restarted by the same helper an update uses, when the .NET SDK is installed.
    async Task RebuildAll()
    {
        if(Surface.GameRunning||Surface.RoomGameRunning){Surface.Alert("CLOSE THE GAME","Close the game before building again.");return;}
        if(_updateBusy||Surface.OperationRunning||Surface.IsInstalling)return;
        var p=RequireProject();
        string script=p.PathIn("tools",p.Platform==DesktopPlatform.Windows?"rebuild-all.ps1":"build_game.py");
        if(!File.Exists(script)){Surface.Alert("OPENSMASH","Build script not found: "+script);return;}
        _updateBusy=true;Surface.SetOperationLabel("BUILDING THE GAME");Surface.OperationRunning=true;Surface.InvalidateVisual();
        string failure="";bool launcher=false;
        try
        {
            await EnsureToolsPython();
            Command command=p.Platform==DesktopPlatform.Windows
                ?new("powershell.exe",["-NoLogo","-NoProfile","-ExecutionPolicy","Bypass","-File",script,"-GameOnly"],p.Root)
                :new(p.Python,["-u",script],p.Root);
            await RunUpdateStep(command,"rebuild_all.log");
            launcher=CanBuildSharedLauncher()&&await DotnetSdkAvailable();
        }
        catch(Exception error){failure=error.Message;}
        finally{_updateBusy=false;Surface.OperationRunning=false;Surface.InvalidateVisual();}
        if(_closed)return;
        if(failure.Length>0){Surface.Alert("BUILD DID NOT FINISH",failure+"\n\nThe log is build/logs/rebuild_all.log.");return;}
        if(launcher){try{ScheduleLauncherBuildAndRestart();}catch(Exception error){Surface.Alert("LAUNCHER BUILD PENDING",error.Message);}return;}
        Surface.SetProject(p);
        Surface.Alert("GAME BUILT","The game was rebuilt. The launcher was not: install the .NET 8 SDK to rebuild it, or use the release launcher.");
    }
}
