using System.Drawing;
using System.Text.Json;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using OpenSmash.Core;

namespace OpenSmash.Visuals;

public sealed partial class ClassicSurface
{
    Setting[] _settings=[];
    SettingTab[] _settingsTabs=[];
    readonly Dictionary<string,string> _iniValues=new();
    readonly HashSet<string> _changedSettings=[];
    readonly Dictionary<string,Rectangle> _editRects=new();
    readonly TextBox _settingEdit=new() { IsVisible=false,Background=Avalonia.Media.Brush.Parse("#121214"),
        Foreground=Avalonia.Media.Brush.Parse("#F4F4F2"),BorderThickness=new Thickness(0),CornerRadius=new CornerRadius(0),MaxLength=64,Padding=new Thickness(4,2) };
    Rect _settingRect;
    int _settingsTab,_ddFirst=-1,_dropdownHoverIndex=-1;
    string? _openDropdownKey,_editingKey;
    bool _settingsLoaded,_volTrackValid;
    float _settingsRowH,_settingsViewTop,_settingsViewH,_settingsMaxScroll,_settingsScroll,_settingsTrackTop,_settingsTrackH,_settingsThumbH;
    float[] _sectionTop=[],_rowTop=[];
    float _volTrackX,_volTrackW,_volTrackY,_volTrackH;
    double _ddAt;
    List<ModLine> _modLines=[];
    readonly HashSet<string> _knownGroups=[],_openGroups=[];
    readonly List<(string Name,string Version,bool Fetched,string Status,string Source)> _stdPlugins=[];
    int _setupProgress;
    string _setupStep="",_setupLog="";
    bool _setupModding;
    readonly Dictionary<string,Bitmap> _modThumbs=new();
    bool _modsChecked;
    int _modScroll,_modMaxScroll;
    float _modTrackTop,_modTrackH,_modThumbH;
    int ModActionCount=>4;
    float ModListTop=>368*_s;
    float ModListView=>Math.Max(68,(ClientSize.Height-118*_s-ModListTop)/_s);
    System.Drawing.Size ClientSize=>new((int)Math.Round(Bounds.Width*_pixelScale),(int)Math.Round(Bounds.Height*_pixelScale));
    string _storageText="",_updateState="",_updateMessage="",_updateDetail="",_updateLocal="",_updateHead="";
    bool _storageBusy,_updateApplying,_updateChecking;
    int _updateBehind;

    void InitializeDetails()
    {
        InitializeSettings();LogicalChildren.Add(_settingEdit);VisualChildren.Add(_settingEdit);
        AttachedToVisualTree+=(_,_)=>ApplyWindowIcon();
        _settingEdit.LostFocus+=(_,_)=>CommitEdit();
        _settingEdit.KeyDown+=(_,e)=> { if(e.Key is Key.Enter or Key.Escape){CommitEdit();Focus();e.Handled=true;} };
    }
    void PaintDetails(Graphics g,int W,int H)
    {
        if(_view=="settings")PaintSettings(g,W,H);else if(_view=="setup_modding")PaintSetupModding(g,W,H);else if(_view=="mods_browser")PaintMarketplace(g,W,H);else PaintMods(g,W,H);
    }
    GraphicsState PushReveal(Graphics g,int i,bool repeat=true)=>PushFade(g,ArtworkTheme.Ease(Clamp01(((float)(Now-_viewAt)-.2f-(repeat?i%6:i)*.07f)/.9f)),40*_s);
    void FitSettingEdit(Rectangle rect)
    {
        _settingEdit.FontSize=15*_s/_pixelScale;_settingEdit.Width=Math.Max(1,(rect.Width-24*_s)/_pixelScale);_settingEdit.Height=Math.Max(1,(rect.Height-8*_s)/_pixelScale);
        var r=new Rect((rect.X+12*_s)/_pixelScale,(rect.Y+4*_s)/_pixelScale,_settingEdit.Width,_settingEdit.Height);if(r!=_settingRect){_settingRect=r;InvalidateArrange();}
    }
    void LoadSettings() { if(Project!=null)ReadSettings(); }
    public string GetSetting(string key)=>_iniValues.GetValueOrDefault(key,"");
    public bool ValidateSettingsForPlay()
    {
        CommitEdit();if(!_settingsLoaded)LoadSettings();
        string lobby=GetSetting("lobby"),code=GetSetting("code").Replace(" ",""),host=GetSetting("server").Trim();
        if(lobby=="private"&&code==""){Alert("TYPE A LOBBY CODE","Type a lobby code: a word your friends type too.");return false;}
        if(lobby=="custom"&&host!=""&&!System.Text.RegularExpressions.Regex.IsMatch(host,@"^\S+:\d+$")){Alert("CHECK YOUR SERVER","Your own server is host:port, for example 192.168.1.20:39420.");return false;}
        try{WriteSettingsFiles();return true;}catch(Exception error){Alert("COULD NOT SAVE",error.Message);return false;}
    }
    public void ChangeSetting(string key,string value)
    {
        if(Project==null)return;
        if(!_settingsLoaded)LoadSettings();
        if(key=="theme_preset") { ApplyThemePreset(value); return; }
        if(key.StartsWith("theme_")) { ChangeThemeColor(key,value); return; }
        if(GameRunning) { Alert("CLOSE THE GAME","Close the game before changing its settings.");return; }
        _changedSettings.Add(key);
        _iniValues[key]=value;
        if(key=="window_size"&&value.Split('x') is {Length:2} parts) {_iniValues["width"]=parts[0];_iniValues["height"]=parts[1];_changedSettings.UnionWith(["width","height"]);}
        try { WriteSettingsFiles(); }catch(Exception error){Alert("COULD NOT SAVE",error.Message);}
        InvalidateVisual();
    }
    void ScrollDetailsTo(float y)
    {
        if(_pressed=="modscroll"&&_modTrackH>_modThumbH)_modScroll=(int)(Math.Clamp((y-_modTrackTop-_modThumbH/2)/(_modTrackH-_modThumbH),0,1)*_modMaxScroll);
        if(_pressed=="setscroll"&&_settingsTrackH>_settingsThumbH)_settingsScroll=Math.Clamp((y-_settingsTrackTop-_settingsThumbH/2)/(_settingsTrackH-_settingsThumbH),0,1)*_settingsMaxScroll;
        InvalidateVisual();
    }
    void CommitEdit()
    {
        if(_editingKey==null)return;string key=_editingKey;_editingKey=null;_settingEdit.IsVisible=false;
        ChangeSetting(key,(_settingEdit.Text??"").Trim());
    }
    bool ActivateDetails(string id)
    {
        if(_modal!=null)return false;
        if(id.StartsWith("modopen:")&&int.TryParse(id[8..],out int row)&&row>=0&&row<_modLines.Count)
        {string group=_mods.FirstOrDefault(m=>_modLines[row].Names.Contains(m.Name))?.Group??"";if(!_openGroups.Remove(group))_openGroups.Add(group);BuildModLines();return true;}
        if(id.StartsWith("modpick:dd:")&&int.TryParse(id[11..],out int pick)&&pick>=0&&pick<_modLines.Count)
        {_openDropdownKey="modpick:"+_modLines[pick].GroupKey;_ddAt=Now;_ddFirst=-1;_dropdownHoverIndex=-1;return true;}
        if(id.StartsWith("modpick:opt:")&&int.TryParse(id[12..],out int variant))
        {var line=_modLines.FirstOrDefault(m=>"modpick:"+m.GroupKey==_openDropdownKey);if(line!=null&&variant>=0&&variant<line.Names.Length)ActionRequested?.Invoke("modvariant:"+line.Names[variant]);_openDropdownKey=null;return true;}
        if(id.StartsWith("tab:")&&int.TryParse(id[4..],out int tab))
        {CommitEdit();_settingsTab=tab;_settingsScroll=0;_openDropdownKey=null;return true;}
        if(id.StartsWith("set:")&&int.TryParse(id.Split(':')[1],out int index)&&index>=0&&index<_settings.Length)
        {
            var setting=_settings[index];var words=id.Split(':');
            if(setting.Key=="theme_reset"){ResetTheme();return true;}
            if(setting.Key=="theme_save"){_ = NameThemePreset();return true;}
            if(ThemeDefaults().Any(item=>item.Key==setting.Key)){_ = PickThemeColor(setting.Key,setting.Label);return true;}
            if(setting.Action!=null){if(SettingOn(setting))ActionRequested?.Invoke(setting.Key=="preload_shaders"&&_shaderUi?"shaders:cancel":setting.Action);return true;}
            if(words.Length==4&&words[2]=="opt"&&int.TryParse(words[3],out int option)&&option>=0&&option<setting.Values.Length)
            {ChangeSetting(setting.Key,setting.Values[option]);_openDropdownKey=null;return true;}
            if(setting.Key=="pads"){ActionRequested?.Invoke("settings:pads");return true;}
            if(setting.Text&&_editRects.TryGetValue(setting.Key,out var rect))
            {CommitEdit();_editingKey=setting.Key;_settingEdit.Text=_iniValues.GetValueOrDefault(setting.Key,"");FitSettingEdit(rect);_settingEdit.IsVisible=true;_settingEdit.Focus();_settingEdit.SelectAll();return true;}
            if(!setting.Slider){_openDropdownKey=_openDropdownKey==setting.Key?null:setting.Key;_ddAt=Now;_ddFirst=-1;_dropdownHoverIndex=-1;}
            return true;
        }
        return false;
    }
    async Task StorageStatusAsync()
    {
        if(Project==null||_storageBusy)return;_storageBusy=true;_storageText="Looking…";
        try
        {
            var lines=new List<string>();int code=await ProcessRunner.Run(new Command(Project.Python,[Project.PathIn("tools","data_location.py")],Project.Root),lines.Add,CancellationToken.None);
            _storageText=(code==0?"":"Could not read where the folders are:\n")+string.Join("\n",lines);
        }
        catch(Exception error){_storageText=error.Message;}
        finally{_storageBusy=false;InvalidateVisual();}
    }
    public void SetStorageResult(string text) { _storageText=text;_storageBusy=false;InvalidateVisual(); }
    public void SetUpdateResult(string state,string message,string local="",string head="",int behind=0)
    {_updateState=state;_updateMessage=message;_updateLocal=local;_updateHead=head;_updateBehind=behind;_updateChecking=state=="checking";_updateApplying=state=="updating";InvalidateVisual();}
    static string ShortCommit(string value)=>value.Length>8?value[..8]:value;
    public void SetMods(string json)
    {
        using var doc=JsonDocument.Parse(json);
        _plugins=Project!=null&&Directory.Exists(Project.PathIn("skyline","plugins"))?Directory.GetFiles(Project.PathIn("skyline","plugins"),"*.nro").Length:0;
        PluginBuildReason=Str(doc.RootElement,"needs_build");VanillaBuildReason=Str(doc.RootElement,"needs_build_vanilla");PluginRefreshReason=Str(doc.RootElement,"needs_refresh");
        foreach(var thumb in _modThumbs.Values)thumb.Dispose();_modThumbs.Clear();
        _mods.Clear();_modLines.Clear();_stdPlugins.Clear();
        if(doc.RootElement.TryGetProperty("plugins",out var plugins))foreach(var item in plugins.EnumerateArray())_stdPlugins.Add((Str(item,"name"),Str(item,"version"),Bool(item,"fetched")??false,Str(item,"status"),Str(item,"source")));
        if(doc.RootElement.TryGetProperty("mods",out var mods))foreach(var item in mods.EnumerateArray())
        {
            string name=Str(item,"name"),title=Str(item,"title");if(title=="")title=name;
            bool on=Bool(item,"enabled")??true;int files=(int)(Num(item,"files")??0);
            string group=Str(item,"group");
            _mods.Add(new ModInfo{Name=name,Title=title,GamebananaId=Str(item,"gamebanana"),Enabled=on,Files=files,Group=group,Part=Str(item,"part"),GroupTitle=Str(item,"group_title"),Author=Str(item,"author"),Category=Str(item,"category"),About=Str(item,"about"),Online=Str(item,"online"),Code=Str(item,"code"),NewSlots=Bool(item,"new_slots")??false,Pick=Bool(item,"pick")??false,Variant=Str(item,"variant"),Added=(int)(Num(item,"added")??0),Merged=(int)(Num(item,"merged")??0),Skipped=(int)(Num(item,"skipped")??0),KeptShared=(int)(Num(item,"kept_shared")??0)});
            foreach(var (key,path) in new[]{(name,Str(item,"thumb")),("g:"+group,Str(item,"group_thumb"))})
                if(path!=""&&File.Exists(path)&&!_modThumbs.ContainsKey(key))try{_modThumbs[key]=new Bitmap(File.ReadAllBytes(path));}catch(IOException){}
        }
        BuildModLines();_modsChecked=true;InvalidateVisual();
    }
    public string[] ModNames(int index)=>index>=0&&index<_modLines.Count?_modLines[index].Names:[];
    public bool ModEnabled(int index)=>index>=0&&index<_modLines.Count&&_modLines[index].Enabled;
    public string[] SelectedModNames=>ModNames(_sel-ModActionCount);
    public void PluginActivity(bool running,string text="",bool failed=false)
    {_setupModding=running;_setupStep=running?"Downloading the community plugins":failed?"Download stopped":"Community plugins are ready";if(running&&text==""){_setupLog="";_setupProgress=0;}if(text!=""){_setupLog+=text+"\n";_setupProgress=Math.Min(95,_setupProgress+10);}if(!running&&!failed)_setupProgress=100;InvalidateVisual();}
    public void SelectMod(string id) {if(int.TryParse(id.Split(':').Last(),out int row))_sel=row;InvalidateVisual();}
    public void ShowFileCheck(JsonElement result)
    {
        string problem=Str(result,"problem");
        var lines=result.TryGetProperty("summary",out var summary)&&summary.ValueKind==JsonValueKind.Array?summary.EnumerateArray().Select(v=>v.GetString()??"").ToList():new List<string>();
        if(problem!="")lines.Add(problem);
        _gameOk=result.TryGetProperty("program",out var program)&&program.ValueKind==JsonValueKind.Object;
        _gameStatus=_gameOk==true?"Game files checked and ready":problem!=""?problem:"No supported game program found";
        Alert(_gameOk==true?"FILES READY":"CHECK YOUR GAME FILES",string.Join("\n",lines));
    }
}
