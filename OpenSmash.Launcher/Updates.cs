using System.Net.Http;
using System.Text.Json;
using System.Text.RegularExpressions;
using OpenSmash.Core;
namespace OpenSmash;
public sealed partial class MainWindow
{
    bool _updateBusy;
    string _updateStatus="";
    const string UpdateApi="https://api.github.com/repos/OpenSmash-Ultimate/OpenSmash-Ultimate";
    async Task<JsonDocument> GitHub(string path)
    {
        using var request=new HttpRequestMessage(HttpMethod.Get,UpdateApi+path);
        request.Headers.UserAgent.ParseAdd("OpenSmash-Launcher/1.0");
        using var response=await Http.SendAsync(request);response.EnsureSuccessStatusCode();
        return JsonDocument.Parse(await response.Content.ReadAsStringAsync());
    }
    async Task CheckUpdates(bool atBoot=false)
    {
        if(_closed||_updateBusy)return;
        _updateBusy=true;_updateStatus="checking";Surface.SetUpdateResult("checking","");
        string local="",head="",message="";int behind=0;
        try
        {
            local=await LocalSourceCommit();
            if(!Regex.IsMatch(local,@"^[0-9a-fA-F]{40}$"))throw new InvalidOperationException("This copy does not name its source commit. Updates cannot be checked.");
            using var latest=await GitHub("/commits/main");head=latest.RootElement.GetProperty("sha").GetString()??"";
            if(local.Equals(head,StringComparison.OrdinalIgnoreCase)){_updateStatus="uptodate";message="OpenSmash is up to date.";}
            else
            {
                using var comparison=await GitHub("/compare/"+local+"..."+head);
                string status=comparison.RootElement.GetProperty("status").GetString()??"";
                behind=comparison.RootElement.TryGetProperty("ahead_by",out var count)?count.GetInt32():0;
                _updateStatus=status=="ahead"?"available":status is "identical" or "behind"?"uptodate":"error";
                message=status switch {"ahead"=>$"A newer version is on GitHub: {behind} new commit(s).","behind"=>"This copy is newer than GitHub's main branch.","identical"=>"OpenSmash is up to date.",_=>"This copy comes from a different branch. Update it with git."};
            }
        }
        catch(Exception error){_updateStatus="error";message=error.Message;}
        finally{_updateBusy=false;if(!_closed)Surface.SetUpdateResult(_updateStatus,message,local,head,behind);}
        if(atBoot&&!_closed&&_updateStatus=="available"&&Surface.UpdatePromptEnabled&&!Surface.HasPrompt&&!Surface.GameRunning&&!Surface.OperationRunning)
        {
            string choice=await Surface.ShowUpdatePrompt();
            if(choice=="update"&&!_closed)await ApplyUpdate();
        }
    }
    async Task<string> LocalSourceCommit()
    {
        var p=RequireProject();string source=p.PathIn("tools","source_version.txt");
        string local=File.Exists(source)?File.ReadAllText(source).Split(' ')[0].Trim():"";
        if(Directory.Exists(p.PathIn(".git"))||File.Exists(p.PathIn(".git")))
        {var lines=new List<string>();if(await ProcessRunner.Run(new Command("git",["rev-parse","HEAD"],p.Root),lines.Add,CancellationToken.None)==0)local=lines.FirstOrDefault()??local;}
        return local;
    }
    async Task ApplyUpdate()
    {
        if(_closed||_updateBusy||Surface.OperationRunning||Surface.GameRunning)return;
        _updateBusy=true;Surface.SetOperationLabel("UPDATING OPENSMASH");Surface.OperationRunning=true;Surface.SetUpdateResult("updating","Updating OpenSmash...");
        string failure="";bool restart=false;
        var p=RequireProject();
        try
        {
            string before=await LocalSourceCommit(),fingerprint=UpdateState.LauncherFingerprint(p);
            if(!Regex.IsMatch(before,@"^[0-9a-fA-F]{40}$"))throw new InvalidOperationException("This copy does not identify its source version.");
            Command command=p.Platform==DesktopPlatform.Windows
                ?new("powershell.exe",["-NoProfile","-ExecutionPolicy","Bypass","-File",p.PathIn("tools","install_windowsmash.ps1"),"-UpdateOnly"],p.Root)
                :new("/bin/bash",[p.PathIn("tools",p.Platform==DesktopPlatform.MacOS?"mac":"linux","install.sh"),"--update-only","--yes"],p.Root);
            await RunUpdateStep(command,"source_update.log");
            ProjectLayout.Normalize(p.Root);
            string after=await LocalSourceCommit();
            if(!before.Equals(after,StringComparison.OrdinalIgnoreCase))UpdateState.Mark(UpdateState.GameMarker(p),after);
            if(fingerprint!=UpdateState.LauncherFingerprint(p)&&CanBuildSharedLauncher())UpdateState.Mark(UpdateState.LauncherMarker(p),after);
            Surface.SetOperationLabel("UPDATING PYTHON PACKAGES");
            if(p.Platform==DesktopPlatform.Windows)await EnsureToolsPython(force:true);
            else await RunUpdateStep(new Command(p.Python,[p.PathIn("tools","desktop_update.py"),"--python-only"],p.Root),"python_setup.log");
            await RebuildUpdatedGame();
            restart=File.Exists(UpdateState.LauncherMarker(p))&&await DotnetSdkAvailable();
        }
        catch(Exception error){failure=error.Message;}
        finally{_updateBusy=false;Surface.OperationRunning=false;Surface.InvalidateVisual();}
        if(_closed)return;
        if(failure.Length>0){Surface.SetUpdateResult("error","Update did not finish: "+failure);Surface.Alert("UPDATE DID NOT FINISH",failure+"\n\nCompleted work is kept. Pending rebuilds will be retried.");return;}
        if(restart){try{ScheduleLauncherBuildAndRestart();return;}catch(Exception error){Surface.Alert("LAUNCHER UPDATE PENDING",error.Message);return;}}
        await CheckUpdates();
        if(!_closed)Surface.Alert("OPENSMASH UPDATED","Your game files, settings and save were kept. The game rebuild completed. If launcher sources changed, install its new release asset or rebuild it with the .NET SDK.");
    }
}
