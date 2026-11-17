using OpenSmash.Core;
namespace OpenSmash.Visuals;

public sealed partial class ClassicSurface
{
    bool _pluginPrompt,_newModsPrompt,_rebuildPrompt,_shaderPrompt,_updatePrompt,_updateDontShow;
    double _pluginPromptAt,_newModsPromptAt,_rebuildPromptAt,_shaderPromptAt,_updatePromptAt;
    string _pluginPromptKind="update",_pluginPromptText="";
    readonly List<string> _newMods=[];
    TaskCompletionSource<string>? _changeChoice;
    public bool HasPrompt => _modal!=null||_sharePrompt||_checkModsPrompt||_pluginPrompt||_newModsPrompt||_rebuildPrompt||_shaderPrompt||_updatePrompt;
    public string PluginBuildReason {get;private set;}="";
    public string VanillaBuildReason {get;private set;}="";
    public string PluginRefreshReason {get;private set;}="";
    public string[] WaitingPlugins => _mods.Where(m=>m.Enabled&&m.Code=="waiting").Select(m=>m.Title).Distinct().ToArray();
    public Task<string> ShowNewMods(string[] names)
    {_newMods.Clear();_newMods.AddRange(names);_newModsPromptAt=Now;_newModsPrompt=true;return ChangeChoice();}
    public Task<string> ShowPluginPrompt(bool refresh,string text)
    {_pluginPromptKind=refresh?"refresh":"update";_pluginPromptText=text;_pluginPromptAt=Now;_pluginPrompt=true;return ChangeChoice();}
    public Task<string> ShowRebuildPrompt()
    {_rebuildPromptAt=Now;_rebuildPrompt=true;return ChangeChoice();}
    public Task<string> ShowShaderPrompt()
    {_shaderPromptAt=Now;_shaderPrompt=true;return ChangeChoice();}
    public bool UpdatePromptEnabled=>Project!=null&&!string.Equals(IniValue(Project.ConfigFile("launcher.ini"),"opensmash","update_prompt"),"off",StringComparison.OrdinalIgnoreCase);
    public Task<string> ShowUpdatePrompt()
    {_updateDontShow=false;_updatePromptAt=Now;_updatePrompt=true;return ChangeChoice();}
    Task<string> ChangeChoice()
    {
        _changeChoice=new(TaskCreationOptions.RunContinuationsAsynchronously);
        UpdateNativeControls();InvalidateVisual();return _changeChoice.Task;
    }
    TaskCompletionSource<string>? _importChoice;
    public Task<string> ChooseModImport()
    {
        _importChoice=new(TaskCreationOptions.RunContinuationsAsynchronously);
        _modal=new Prompt{Title="ADD MOD",Body="Copy a mod folder into this project, or add it as a symbolic link so changes to the original folder are shared. A link needs its original folder to stay in place. You choose the folder next.",Yes="COPY",Mid="ADD AS LINK",No="CANCEL",OnYes=()=>_importChoice?.TrySetResult("copy"),OnMid=()=>_importChoice?.TrySetResult("link"),OnNo=()=>_importChoice?.TrySetResult("cancel")};
        UpdateNativeControls();InvalidateVisual();return _importChoice.Task;
    }
    public void CancelChangePrompt(){_importChoice?.TrySetResult("cancel");FinishChangeChoice("cancel");}
    void FinishChangeChoice(string choice)
    {
        if(_updatePrompt&&_updateDontShow&&Project!=null)SetIni(Project.ConfigFile("launcher.ini"),"opensmash","update_prompt","off");
        _pluginPrompt=_newModsPrompt=_rebuildPrompt=_shaderPrompt=_updatePrompt=false;
        var completion=_changeChoice;_changeChoice=null;
        UpdateNativeControls();InvalidateVisual();completion?.TrySetResult(choice);
    }
    bool ModHasPlugin(string name)=>Project!=null&&new ModChanges(Project).HasPlugin(name);
    bool ActivateChangePrompt(string id)
    {
        if(!_pluginPrompt&&!_newModsPrompt&&!_rebuildPrompt&&!_shaderPrompt&&!_updatePrompt)return false;
        if(id=="close"){ActionRequested?.Invoke(id);return true;}
        if(_pluginPrompt&&id.StartsWith("plugin_prompt:"))
        {if(id=="plugin_prompt:build")FinishChangeChoice(_pluginPromptKind=="refresh"?"refresh":"build");else if(id.EndsWith(":play"))FinishChangeChoice("play");else if(id.EndsWith(":cancel"))FinishChangeChoice("cancel");}
        else if(_newModsPrompt&&id=="newmods:build")FinishChangeChoice("enable");
        else if(_rebuildPrompt&&id.StartsWith("rebuild_prompt:")&&!id.EndsWith(":block"))FinishChangeChoice(id.Split(':')[1]=="update"?"build":id.Split(':')[1]);
        else if(_shaderPrompt&&id.StartsWith("shader_prompt:")&&!id.EndsWith(":block"))FinishChangeChoice(id.Split(':')[1]);
        else if(_updatePrompt&&id=="update_prompt:dont"){_updateDontShow=!_updateDontShow;InvalidateVisual();}
        else if(_updatePrompt&&id.StartsWith("update_prompt:")&&!id.EndsWith(":block"))FinishChangeChoice(id.Split(':')[1]);
        return true;
    }
}
