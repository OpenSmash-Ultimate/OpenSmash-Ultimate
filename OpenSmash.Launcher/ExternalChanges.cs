using Avalonia.Platform.Storage;
using OpenSmash.Core;

namespace OpenSmash;
public sealed partial class MainWindow
{
    bool _opened,_closed,_checkingChanges,_playPreflight,_playQueued;
    string _dismissedPluginState="";
    async Task CheckExternalChanges(bool atBoot=false,bool force=false)
    {
        if(_closed||_checkingChanges||_playPreflight||_project==null||!_project.IsBuilt||Surface.IsInstaller||Surface.HasPrompt||Surface.OperationRunning||Surface.GameRunning)return;
        _checkingChanges=true;
        try
        {
            if(atBoot)
            {
                await Task.Delay(850);
                if(TryResumePendingLauncherBuild())return;
                await EnsureToolsPython();
            }
            if(_closed||Surface.IsInstaller||Surface.HasPrompt)return;
            await PrepareExternalChanges(forPlay:false,force);

        }
        catch(Exception error){if(!_closed)Surface.Alert("COULD NOT CHECK MODS",error.Message);}
        finally{_checkingChanges=false;}
        if(_playQueued){_playQueued=false;if(!_closed&&!Surface.HasPrompt)await Action("play");}
        if(atBoot&&!_closed&&!Surface.IsInstaller&&!Surface.HasPrompt)
        {
            try{await AskShaders();if(!_closed)await CheckUpdates(atBoot:true);}
            catch(Exception error){if(!_closed)Surface.Alert("STARTUP CHECK",error.Message);}
        }
    }
    async Task<bool> PrepareExternalChanges(bool forPlay,bool force=false)
    {
        if(forPlay&&!await FinishPendingGameUpdate())return false;
        var changes=new ModChanges(RequireProject());
        // Snapshot BEFORE Python status: its Tree.load can register newly found folders.
        string[] added=changes.Scan();
        await RefreshMods();
        if(_closed)return false;
        if(added.Length>0)
        {
            string choice=await Surface.ShowNewMods(added);
            if(choice!="enable"||_closed)return false;
            await ModCommand("enable",added); // Acknowledge only once the backend succeeds.
        }
        bool vanilla=forPlay&&Surface.LaunchMode=="vanilla";
        string buildReason=vanilla?Surface.VanillaBuildReason:Surface.PluginBuildReason;
        bool full=buildReason.Length>0;
        if(vanilla&&!full)return true;
        var waiting=vanilla?new List<string>():Surface.WaitingPlugins.ToList();
        string why=full?PluginUpdateWhy(waiting,buildReason):PluginRefreshWhy(waiting,Surface.PluginRefreshReason);
        if(why.Length==0){_dismissedPluginState="";return true;}
        string state=RequireProject().Root+"\n"+full+"\n"+Surface.PluginBuildReason+"\n"+Surface.PluginRefreshReason+"\n"+why;
        if(!forPlay&&!force&&_dismissedPluginState==state)return true;
        string action=await Surface.ShowPluginPrompt(!full,why);
        if(_closed)return false;
        if(action is "build" or "refresh")
        {
            await ModCommand(action);
            _dismissedPluginState="";
            return false; // Original launcher returns to Play after a successful rebuild.
        }
        if(action=="cancel")_playQueued=false;
        _dismissedPluginState=state;
        return action=="play";
    }
    async Task AddPluginFiles()
    {
        var files=await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {Title="Add plugins",AllowMultiple=true,FileTypeFilter=[new FilePickerFileType("Plugin (*.nro)"){Patterns=["*.nro"]}]});
        string folder=RequireProject().PathIn("skyline","plugins");Directory.CreateDirectory(folder);
        foreach(var file in files)
            if(file.TryGetLocalPath() is string source&&!Path.GetFullPath(source).Equals(Path.Combine(folder,Path.GetFileName(source)),StringComparison.OrdinalIgnoreCase))
                File.Copy(source,Path.Combine(folder,Path.GetFileName(source)),true);
        if(files.Count>0)await CheckExternalChanges(force:true);
    }
}
