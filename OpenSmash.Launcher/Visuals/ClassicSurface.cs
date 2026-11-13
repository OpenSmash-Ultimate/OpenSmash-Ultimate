using System.Diagnostics;
using System.Drawing;
using System.Text.Json;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Avalonia.Threading;
using OpenSmash.Core;
using SkiaSharp;

namespace OpenSmash.Visuals;

public sealed partial class ClassicSurface : Control
{
    public Project? Project { get; private set; }
    public event Action<string>? ActionRequested;
    public event Action<PointerPressedEventArgs>? DragRequested;
    public event Action<string>? InputRequested;
    public bool IsInstaller { get; private set; } = true;
    public bool IsInstalling => _running;
    public bool GameRunning { get; set; }
    bool _startingGame;
    double _startingAt;
    public bool StartingGame
    {
        get=>_startingGame;
        set{if(value&&!_startingGame){_startingAt=Now;_loadProgress=.05f;}_startingGame=value;_loadOn=value||GameRunning||ModsRunning;_loadLabel=value?"STARTING THE GAME":OperationRunning?_operationLabel:GameRunning?"Game running":"Mod manager open";InvalidateVisual();}
    }
    public bool ModsRunning { get; set; }
    public bool OperationRunning { get; set; }
    public string GameFolder => _gamePath;
    public string KeysFolder => _keysPath;
    public string FirmwareFolder => _fwPath;
    public string Username => _username.Text?.Trim() ?? "";
    public bool Shortcuts => _optShortcuts;
    public bool Firewall => _optFirewall;
    public bool Shaders => _optShaders;
    public bool Updates => _optUpdates;
    public bool AcceptLicense => _optLicense;
    public int Jobs => _jobs;
    public string LaunchMode => _launchMode;
    public string CurrentView => IsInstaller ? "installer:"+_view : _view;
    public string CurrentStep => _steps.FirstOrDefault(s=>s.State=="run")?.Title ?? "";
    public double Progress => _steps.FirstOrDefault(s=>s.State=="run") is {Total:>0} run ? run.Done/run.Total : 0;
    public IReadOnlyList<string> LogLines => _log;
    double? _frozenTime;
    public double? FrozenTime // deterministic artwork verification; null in normal use
    {
        get=>_frozenTime;
        set {if(_frozenTime.HasValue!=value.HasValue){_rng=value.HasValue?new Random(241):new Random();_fieldSize=System.Drawing.Size.Empty;}_frozenTime=value;if(value.HasValue){_lastTick=value.Value;_pendingDt=0;}}
    }

    readonly TextBox _username = new() { PlaceholderText="type username here", MaxLength=32,
        Background=Avalonia.Media.Brush.Parse("#101012"), Foreground=Avalonia.Media.Brush.Parse("#ECECEC"),
        BorderBrush=Avalonia.Media.Brush.Parse("#555555"), BorderThickness=new Thickness(1), CornerRadius=new CornerRadius(0), Padding=new Thickness(1,0), MinHeight=0, VerticalContentAlignment=Avalonia.Layout.VerticalAlignment.Top };
    readonly Stopwatch _clock = Stopwatch.StartNew();
    readonly DispatcherTimer _timer;
    WriteableBitmap? _frame;
    double _pixelScale=1;
    Bitmap? _bg, _logo;
    Bitmap? _slashCache;
    const int SlashMargin=40;
    bool _backgroundHome;
    float _s=1, _alpha=1, _tx, _ty, _mx, _my;
    double Now => FrozenTime ?? _clock.Elapsed.TotalSeconds;
    double _shownAt=0, _heroAt=0, _viewAt=0, _lastTick, _pendingDt;
    string _view="setup", _hover="", _pressed="", _launchMode="mods", _version="";
    bool _built, _keys;
    int _sel, _plugins;
    bool Busy => GameRunning || ModsRunning || OperationRunning || _setupModding || _running;
    string PlatformCaption => Project?.Platform switch { DesktopPlatform.MacOS=>"MACOS ARM64", DesktopPlatform.Linux=>"LINUX X64", _=>"WINDOWS X64" };
    string PlatformName => Project?.Platform switch { DesktopPlatform.MacOS=>"macOS",DesktopPlatform.Linux=>"Linux",_=>"Windows" };
    const float Gutter=96, HeaderH=64, StatusH=40, SectionTitleTop=112, ContentTop=184;
    readonly string[] _menuItems=["PLAY","MODS","SETTINGS","QUIT"];
    readonly List<ModInfo> _mods=[];
    readonly Dictionary<(int,int),Font> _fontCache=new();
    readonly Dictionary<(string,int),GraphicsPath> _pathCache=new();
    readonly List<(string Id, RectangleF R)> _hits=[];
    readonly Dictionary<string,string> _tips=new();
    readonly Dictionary<string,float> _hov=new();
    readonly HashSet<string> _hovOn=[];
    struct SpeedLine { public float X,Y,Len,V,W,A,Z; public bool Red; }
    struct Spark { public float X,Y,Sz,Vy,Vx,A,Z; public bool Red; }
    SpeedLine[] _lines=[];
    Spark[] _sparks=[];
    Random _rng=new();
    System.Drawing.Size _fieldSize;
    Rect _usernameRect;
    float Rnd()=>(float)_rng.NextDouble();
    static readonly float Cos14=MathF.Cos(-14*MathF.PI/180), Sin14=MathF.Sin(-14*MathF.PI/180);
    static readonly (float Top,float H,float Delay,float Op,bool Red)[] Slashes=[
        (.30f,3,.15f,1,false),(.34f,18,.30f,.95f,false),(.66f,6,.45f,1,true),(.71f,1,.60f,.7f,false),(.12f,1,.75f,.4f,false)];
    // Active job strip fields are shared with the original status-strip drawing.
    bool _loadOn;
    string _loadLabel="";
    string _operationLabel="WORKING";
    public void SetOperationLabel(string label){_operationLabel=label;_loadLabel=label;_loadProgress=-1;InvalidateVisual();}
    float _loadProgress=-1;

    string _gamePath="",_keysPath="",_fwPath="";
    string _gameStatus="Not chosen",_keysStatus="Not chosen",_fwStatus="Optional: open-source fonts will be used";
    bool? _gameOk,_keysOk,_fwOk=true;
    bool _optShortcuts=true,_optFirewall=true,_optShaders=true,_optUpdates=true,_optLicense=true;
    int _jobs=Math.Max(1,Math.Min(24,Environment.ProcessorCount-2));
    sealed class Step
    {
        public string Id="",Title="",State="wait",Msg="";
        public double Est=10,Secs,Done,Total;
        public bool EstP;
        public DateTime? T0;
    }
    readonly List<Step> _steps=[];
    readonly List<string> _log=[];
    readonly HashSet<string> _genModules=new(StringComparer.OrdinalIgnoreCase);
    int _genTotal,_fallbackN,_logScroll;
    bool _jsonSeen,_running,_succeeded,_failedBefore,_jobDry=false;
    string _jobKind="install",_errMsg="",_errHint="",_errAction="",_doneMsg="";
    bool? _doneOk;
    DateTime _jobStart,_jobEnd;
    string _status="Looking for your game files…";
    Color _statusColor=ArtworkTheme.Muted;
    bool Installed => Project?.IsBuilt==true;
    sealed class Prompt
    {
        public string Title="",Body="",Yes="OK",Mid="",No="";
        public Action? OnYes,OnMid,OnNo;
    }
    Prompt? _modal;

    public ClassicSurface()
    {
        Focusable=true;
        SizeChanged+=(_,_)=>UpdateNativeControls();
        _username.FontFamily=new Avalonia.Media.FontFamily(OperatingSystem.IsWindows()?"Segoe UI":"avares://OpenSmash/Assets/Inter-Regular.ttf#Inter");
        _settingEdit.FontFamily=_username.FontFamily;
        ClipToBounds=true;
        LogicalChildren.Add(_username); VisualChildren.Add(_username);
        InitializeDetails();
        using(var image=AssetLoader.Open(new Uri("avares://OpenSmash/Assets/emblem-white.png")))
        {using var bytes=new MemoryStream();image.CopyTo(bytes);_logo=new Bitmap(bytes.ToArray());}
        _timer=new DispatcherTimer(TimeSpan.FromMilliseconds(15),DispatcherPriority.Render,(_,_)=>Tick());
        AttachedToVisualTree+=(_,_)=>_timer.Start();
        DetachedFromVisualTree+=(_,_)=>_timer.Stop();
    }

    void Tick()
    {
        UpdateNativeControls();
        _loadOn=StartingGame||OperationRunning||GameRunning||ModsRunning;_loadLabel=StartingGame?"STARTING THE GAME":OperationRunning?_operationLabel:GameRunning?"Game running":"Mod manager open";
        double now=Now;
        if(StartingGame)_loadProgress=Math.Min(.92f,.08f+(float)(now-_startingAt)/45f);
        _pendingDt+=Math.Max(0,now-_lastTick);_lastTick=now;
        if(TopLevel.GetTopLevel(this) is Window window)
        {if(window.WindowState==WindowState.Minimized){_pendingDt=0;return;}if(!window.IsActive&&_pendingDt<.05)return;}
        float dt=(float)Math.Min(.1,_pendingDt);_pendingDt=0;
        if(FrozenTime==null) StepField(dt*60);
        foreach(string key in _hov.Keys.ToArray())
        {bool on=_hovOn.Contains(key);float value=_hov[key];value=on?Math.Min(1,value+dt/.3f):Math.Max(0,value-dt/.3f);if(value==0&&!on)_hov.Remove(key);else _hov[key]=value;}
        float k=1-MathF.Pow(.94f,dt*60);_tx+=(_mx-_tx)*k;_ty+=(_my-_ty)*k;
        InvalidateVisual();
    }
    public void StartEntrance(){_shownAt=_heroAt=_viewAt=_lastTick=Now;_pendingDt=0;}
    public void SetProject(Project? project,bool forceInstaller=false)
    {
        bool changed=Project?.Root!=project?.Root,wasBuilt=_built;
        Project=project; _built=project?.IsBuilt==true;
        if(changed || forceInstaller) { IsInstaller=forceInstaller || !_built; _view=IsInstaller?"setup":"home"; _viewAt=_heroAt=Now; LoadFolders(); }
        else if(IsInstaller && _view=="setup" && _built && !forceInstaller) { IsInstaller=false; _view="home"; _viewAt=_heroAt=Now; }
        if(project!=null)
        {
            string version=project.PathIn("version.txt");
            _version=File.Exists(version)?File.ReadLines(version).FirstOrDefault(l=>l.Trim().Length>0 && !l.StartsWith('#') && !l.StartsWith(';'))?.Trim()??"":"";
            string launch=project.PathIn("build","launch_mode.txt");
            if(changed && File.Exists(launch)) _launchMode=File.ReadAllText(launch).Trim()=="vanilla"?"vanilla":"mods";
            _mods.Clear();
            _plugins=Directory.Exists(project.PathIn("skyline","plugins"))?Directory.GetFiles(project.PathIn("skyline","plugins"),"*.nro").Length:0;
            if(Directory.Exists(project.PathIn("mods"))) _mods.AddRange(Directory.GetDirectories(project.PathIn("mods")).Select(path=>new ModInfo{Name=Path.GetFileName(path),Title=Path.GetFileName(path)}));
            if(changed||!wasBuilt&&_built)LoadSettings();
        }
        UpdateNativeControls();InvalidateVisual();
    }
    public void Navigate(string view)
    {
        CommitEdit();
        if(view=="setup") { IsInstaller=true; _view="setup"; }
        else { IsInstaller=false; _view=view; }
        _viewAt=Now; if(view=="home") _heroAt=Now;
        if(view=="settings")LoadSettings();
        if(view=="mods")ActionRequested?.Invoke("mods:refresh");
        _modal=null; InvalidateVisual();
        UpdateNativeControls();
    }
    public void SetFolder(string kind,string path)
    {
        switch(kind) { case "game":_gamePath=path;break;case "keys":_keysPath=path;break;case "firmware":_fwPath=path;break; }
        CheckFolders();
    }
    void LoadFolders()
    {
        if(Project==null) return;
        string saved=Project.PathIn("dump","build_folders.txt");
        if(File.Exists(saved)) foreach(var line in File.ReadLines(saved))
        { int i=line.IndexOf('='); if(i<0)continue; string key=line[..i],value=line[(i+1)..]; if(key=="game")_gamePath=value; if(key=="keys")_keysPath=value; if(key=="firmware")_fwPath=value; }
        string app=Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),home=Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        var bases=new[]{app,Path.Combine(home,".local","share"),Path.Combine(home,"Library","Application Support")};
        foreach(string basePath in bases) foreach(string name in new[]{"yuzu","eden","sudachi","citron","suyu","Ryujinx"})
        {
            string dir=Path.Combine(basePath,name);
            foreach(string key in new[]{Path.Combine(dir,"keys"),Path.Combine(dir,"system")}) if(_keysPath==""&&File.Exists(Path.Combine(key,"prod.keys")))_keysPath=key;
            foreach(string game in new[]{Path.Combine(dir,"nand","user","Contents","registered"),Path.Combine(dir,"bis","user","Contents","registered")}) if(_gamePath==""&&Directory.Exists(game))_gamePath=game;
            foreach(string fw in new[]{Path.Combine(dir,"nand","system","Contents","registered"),Path.Combine(dir,"bis","system","Contents","registered")}) if(_fwPath==""&&Directory.Exists(fw))_fwPath=fw;
        }
        if(_keysPath==""&&File.Exists(Path.Combine(home,".switch","prod.keys")))_keysPath=Path.Combine(home,".switch");
        string online=Project.ConfigFile("online_name.txt"); if(File.Exists(online))_username.Text=File.ReadAllText(online).Trim();
        CheckFolders();
    }
    public bool CheckFolders()
    {
        _gameOk=Directory.Exists(_gamePath);_keysOk=File.Exists(Path.Combine(_keysPath,"prod.keys"));_fwOk=_fwPath==""||Directory.Exists(_fwPath);
        _gameStatus=_gameOk==true?"Game folder selected — Check files verifies the dump":"Choose the folder containing your own dumped game";
        _keysStatus=_keysOk==true?"prod.keys found":"prod.keys was not found in this folder";
        _fwStatus=_fwPath==""?"Optional: open-source fonts will be used":_fwOk==true?"Firmware folder selected":"Folder not found";
        _status="Choose your files, type a username, then Set up and build.";
        InvalidateVisual();return _gameOk==true&&_keysOk==true&&_fwOk==true;
    }
    void UpdateNativeControls()
    {
        UpdateMarketplace();
        _username.IsVisible=IsInstaller&&_view=="setup"&&!HasPrompt;
        if(!IsInstaller||_view!="settings"||HasPrompt)_settingEdit.IsVisible=false;
        float s=(float)(Bounds.Height>0?Bounds.Height/720:1);
        double scale=TopLevel.GetTopLevel(this)?.RenderScaling??1;
        _username.FontSize=15*s;
        _username.Width=Math.Max(1,Math.Floor((Bounds.Width-738*s)*scale)/scale);
        _username.Height=UsernameHeight((float)(s*scale))/scale;
    }
    protected override Avalonia.Size MeasureOverride(Avalonia.Size available) { _username.Measure(available);_settingEdit.Measure(available);_marketplace?.Measure(available);return new(double.IsInfinity(available.Width)?1280:available.Width,double.IsInfinity(available.Height)?720:available.Height); }
    float _usernameHeightScale=-1;int _usernameHeight;
    int UsernameHeight(float scale)
    {
        if(_usernameHeightScale!=scale){_usernameHeightScale=scale;_usernameHeight=OperatingSystem.IsWindows()?WindowsRasterizer.TextBoxHeight(15*scale):(int)Math.Ceiling(20*scale)+7;}
        return _usernameHeight;
    }
    protected override Avalonia.Size ArrangeOverride(Avalonia.Size final)
    {
        float s=(float)final.Height/720;double scale=TopLevel.GetTopLevel(this)?.RenderScaling??1;
        _usernameRect=new Rect(Math.Floor(214*s*scale)/scale,Math.Floor(502*s*scale)/scale,Math.Max(1,Math.Floor((final.Width-738*s)*scale)/scale),UsernameHeight((float)(s*scale))/scale);
        _username.Arrange(_usernameRect);_settingEdit.Arrange(_settingRect);ArrangeMarketplace(final);return final;
    }

    public override void Render(Avalonia.Media.DrawingContext context)
    {
        base.Render(context);
        _pixelScale=TopLevel.GetTopLevel(this)?.RenderScaling??1;
        int W=Math.Max(1,(int)Math.Round(Bounds.Width*_pixelScale)),H=Math.Max(1,(int)Math.Round(Bounds.Height*_pixelScale));
        _s=H/720f;_built=Project?.IsBuilt==true;
        if(_frame==null||_frame.PixelSize.Width!=W||_frame.PixelSize.Height!=H) { _frame?.Dispose();_frame=new WriteableBitmap(new PixelSize(W,H),new Vector(96*_pixelScale,96*_pixelScale),PixelFormat.Bgra8888,AlphaFormat.Premul); }
        using(var fb=_frame.Lock())
        using(var surface=SKSurface.Create(new SKImageInfo(W,H,SKColorType.Bgra8888,SKAlphaType.Premul),fb.Address,fb.RowBytes))
        using(var g=Graphics.ForFrame(surface.Canvas,W,H,fb.Address,fb.RowBytes))
        {
            g.GridFit=IsInstaller;
            bool home=!IsInstaller&&_view=="home";
            if(_bg==null||_bg.Width!=W||_bg.Height!=H||_backgroundHome!=home) { _bg?.Dispose();_bg=ArtworkTheme.MakeBackground(W,H,null,!home);_backgroundHome=home; }
            g.DrawImageUnscaled(_bg,0,0);_hits.Clear();_tips.Clear();_hovOn.Clear();
            if(IsInstaller)
            {
                if(IsFirstRun)PaintBootstrap(g,W,H);else if(_view=="build") PaintBuild(g,W,H);else PaintSetup(g,W,H);
                PaintFooter(g,W,H);ArtworkTheme.CloseButton(g,W,_s,_hover=="close",out var close);_hits.Add(("close",close));
            }
            else
            {
                EnsureField(W,H);DrawField(g,W,H,home?1:.55f);
                if(home) { DrawSlashes(g,W,H,(float)(Now-_shownAt));PaintHome(g,W,H); }
                else PaintDetails(g,W,H);
                PaintHeader(g,W,H);if(_loadOn&&_view!="mods")PaintLoad(g,W,H);PaintStatus(g,W,H);
            }
            if(_pluginPrompt)PaintPluginPrompt(g,W,H);
            if(_newModsPrompt)PaintNewModsPrompt(g,W,H);
            if(_rebuildPrompt)PaintRebuildPrompt(g,W,H);
            if(_shaderPrompt)PaintShaderPrompt(g,W,H);
            if(_updatePrompt)PaintUpdatePrompt(g,W,H);
            if(_sharePrompt)PaintSharePrompt(g,W,H);
            if(_checkModsPrompt)PaintCheckModsPrompt(g,W,H);
            if(_modal!=null)PaintModal(g,W,H);
            surface.Canvas.Flush();
        }
        context.DrawImage(_frame,new Rect(0,0,Bounds.Width,Bounds.Height));
    }
    string HitAt(Avalonia.Point p)=>_hits.LastOrDefault(h=>h.R.Contains((float)(p.X*_pixelScale),(float)(p.Y*_pixelScale))).Id??"";

    protected override void OnPointerMoved(PointerEventArgs e)
    {
        base.OnPointerMoved(e);var p=e.GetPosition(this);_keys=false;
        _hover=HitAt(p);
        _mx=(float)(p.X/Math.Max(1,Bounds.Width)-.5)*2;_my=(float)(p.Y/Math.Max(1,Bounds.Height)-.5)*2;
        Cursor=new Cursor(_hover==""||_hover=="log"||_hover.StartsWith("step:")?StandardCursorType.Arrow:StandardCursorType.Hand);
        if(_pressed is "modscroll" or "setscroll")ScrollDetailsTo((float)(p.Y*_pixelScale));
        if(_pressed=="vol:volume"&&_volTrackValid)ChangeSetting("volume",Math.Clamp((int)Math.Round(((float)(p.X*_pixelScale)-_volTrackX)/_volTrackW*100),0,100).ToString());
        if(_hover.Contains(":opt:")&&int.TryParse(_hover.Split(':').Last(),out int option))_dropdownHoverIndex=option;
        InvalidateVisual();
    }
    protected override void OnPointerExited(PointerEventArgs e) { _hover="";_mx=_my=0;base.OnPointerExited(e); }
    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        base.OnPointerPressed(e);if(!e.GetCurrentPoint(this).Properties.IsLeftButtonPressed)return;
        var p=e.GetPosition(this);_pressed=HitAt(p);
        if(_pressed is "modscroll" or "setscroll")ScrollDetailsTo((float)(p.Y*_pixelScale));
        if(_pressed=="vol:volume"&&_volTrackValid)ChangeSetting("volume",Math.Clamp((int)Math.Round(((float)(p.X*_pixelScale)-_volTrackX)/_volTrackW*100),0,100).ToString());
        if(_pressed==""&&_modal==null)DragRequested?.Invoke(e);else { Focus();e.Pointer.Capture(this); }
        InvalidateVisual();
    }
    protected override void OnPointerReleased(PointerReleasedEventArgs e)
    {
        base.OnPointerReleased(e);var p=e.GetPosition(this);string hit=HitAt(p);
        string pressed=_pressed;_pressed="";e.Pointer.Capture(null);if(hit!=""&&hit==pressed)Activate(hit);InvalidateVisual();
    }
    protected override void OnPointerWheelChanged(PointerWheelEventArgs e)
    {
        if(IsInstaller)_logScroll=Math.Max(0,_logScroll+(int)(e.Delta.Y*3));
        else if(_view=="settings")_settingsScroll=Math.Clamp(_settingsScroll-(float)e.Delta.Y*40*_s,0,_settingsMaxScroll);
        else if(_view=="mods")_modScroll=Math.Clamp(_modScroll-(int)(e.Delta.Y*40),0,_modMaxScroll);
        InvalidateVisual();base.OnPointerWheelChanged(e);
    }
    protected override void OnKeyDown(KeyEventArgs e)
    {
        base.OnKeyDown(e);if(_username.IsFocused)return;
        if(_pluginPrompt||_newModsPrompt||_rebuildPrompt||_shaderPrompt||_updatePrompt)
        {
            if(e.Key==Key.Escape&&!_newModsPrompt)FinishChangeChoice("cancel");
            else if(e.Key==Key.Enter&&_newModsPrompt)FinishChangeChoice("enable");
            else if(e.Key==Key.Enter&&_rebuildPrompt)FinishChangeChoice("refresh");
            else if(e.Key==Key.Enter&&_updatePrompt)FinishChangeChoice("update");
            e.Handled=true;return;
        }
        if(IsFirstRun&&e.Key==Key.Escape&&_modal==null) { ActionRequested?.Invoke(_bootstrapBusy?"bootstrap:cancel":"close");e.Handled=true;return; }
        if(e.Key==Key.Escape&&(_sharePrompt||_checkModsPrompt)){_sharePrompt=_checkModsPrompt=false;InvalidateVisual();e.Handled=true;return;}
        if(!IsInstaller&&_view=="settings"&&_modal==null&&HandleSettingsKey(e.Key)){e.Handled=true;InvalidateVisual();return;}
        if(e.Key==Key.Escape) { if(_modal!=null){var dismissed=_modal;_modal=null;dismissed.OnNo?.Invoke();}else if(!IsInstaller)Navigate("home");e.Handled=true; }
        if(_modal!=null){e.Handled=true;return;}
        if(!IsInstaller&&_view=="mods"&&HandleModsKey(e.Key)){e.Handled=true;InvalidateVisual();return;}
        if(!IsInstaller&&_view=="home") { if(e.Key==Key.Left||e.Key==Key.Right){_keys=true;_sel=(_sel+(e.Key==Key.Left?3:1))%4;}if(e.Key==Key.Enter)Activate("menu:"+_sel); }

    }
    public void Activate(string id)
    {
        if(StartingGame&&id is not ("close" or "min")&&!id.StartsWith("modal:"))return;
        if(_modal==null&&ActivateChangePrompt(id))return;
        if(_modal==null&&ActivateModDialog(id))return;
        if(ActivateDetails(id)) { InvalidateVisual();return; }
        if(id.StartsWith("modal:")) { var prompt=_modal;_modal=null;switch(id){case "modal:yes":prompt?.OnYes?.Invoke();break;case "modal:mid":prompt?.OnMid?.Invoke();break;case "modal:no":prompt?.OnNo?.Invoke();break;} }
        else if(id.StartsWith("opt:")) { switch(id){case "opt:shortcuts":_optShortcuts=!_optShortcuts;break;case "opt:firewall":_optFirewall=!_optFirewall;break;case "opt:shaders":_optShaders=!_optShaders;break;case "opt:updates":_optUpdates=!_optUpdates;break;case "opt:license":_optLicense=!_optLicense;break;} }
        else if(id=="jobs:-")_jobs=Math.Max(1,_jobs-1);
        else if(id=="jobs:+")_jobs=Math.Min(64,_jobs+1);
        else if(id is "nav:mode-prev" or "nav:mode-next") { _launchMode=_launchMode=="mods"?"vanilla":"mods";if(Project!=null){Directory.CreateDirectory(Project.PathIn("build"));File.WriteAllText(Project.PathIn("build","launch_mode.txt"),_launchMode+"\n");} }
        else if(id=="nav:back")Navigate(_view is "mods_browser" or "setup_modding"?"mods":"home");
        else if(id is "nav:home" or "brand")Navigate("home");
        else if(id is "nav:mods" or "menu:1")Navigate("mods");
        else if(id is "nav:settings" or "menu:2")Navigate("settings");
        else if(id is "menu:3")ActionRequested?.Invoke("close");
        else if(id is "menu:0" or "nav:play")ActionRequested?.Invoke("play");
        else if(id=="act:back")Navigate("setup");
        else if(id=="act:continue") { if(Installed){Navigate("home");ActionRequested?.Invoke("project:ready");}else ActionRequested?.Invoke("close"); }
        else ActionRequested?.Invoke(id);
        UpdateNativeControls();InvalidateVisual();
    }
    public void Alert(string title,string message) { _modal=new Prompt{Title=title,Body=message};UpdateNativeControls();InvalidateVisual(); }
    public void Confirm(string title,string message,Action yes) { _modal=new Prompt{Title=title,Body=message,Yes="YES",No="CANCEL",OnYes=yes};UpdateNativeControls();InvalidateVisual(); }
    public void BeginInstall()
    {
        ResetSteps();_running=true;_succeeded=false;_jobStart=DateTime.Now;_view="build";IsInstaller=true;_log.Clear();_status="Working… this can take from 30 minutes to a few hours.";UpdateNativeControls();InvalidateVisual();
    }
    public void ReceiveInstallLine(string line)
    {
        if(line.Trim()=="SHADER_PRIORITY?") { _modal=new Prompt{Title="SHADER COMPILE",Body="Shaders compile in this window, and the bar shows how far along they are. Low keeps the PC easy to use. Medium is in between. High finishes sooner, and the PC feels slower until it is done.",Yes="HIGH",Mid="MEDIUM",No="LOW",OnYes=()=>InputRequested?.Invoke("high"),OnMid=()=>InputRequested?.Invoke("medium"),OnNo=()=>InputRequested?.Invoke("low")};return; }
        if(line.StartsWith("##ssbu ")) { try {using var doc=JsonDocument.Parse(line[7..]);HandleEvent(doc.RootElement);}catch(JsonException){Log(line);} }
        else { Log(line);if(!_jsonSeen&&!ParseSetupLine(line.Trim()))ApplyMeasure(line.Trim());if(line.Trim().StartsWith("ERROR")||line.StartsWith("FAILED"))_errMsg=line;_status=line; }
        InvalidateVisual();
    }
    public void EndInstall(int exit)
    {
        _modal=null;_running=false;_jobEnd=DateTime.Now;_succeeded=exit==0&&_doneOk!=false&&Installed;_failedBefore=!_succeeded;
        foreach(var step in _steps.Where(s=>s.State=="run"))SetStepState(step,_succeeded?"done":"fail",null,"");
        if(!_succeeded&&_errMsg=="")_errMsg=$"Setup stopped (exit {exit}). See the log, fix the problem, then resume.";
        _status=_succeeded?"The game is built. Press Continue to open the launcher.":_errMsg;InvalidateVisual();
    }
    public void BeginShaderStep()
    {
        foreach(var step in _steps.Where(s=>s.State=="run"))SetStepState(step,"done",null,"");
        var next=AddStep("shaders","Prepare the shaders",600);SetStepState(next,"run",null,"");_jsonSeen=false;
    }
    void Log(string text) { _log.Add(text);if(_log.Count>3000)_log.RemoveRange(0,500); }
    Step? GetStep(string id)=>_steps.FirstOrDefault(s=>s.Id==id);
    static string Str(JsonElement e,string name)=>e.TryGetProperty(name,out var v)&&v.ValueKind==JsonValueKind.String?v.GetString()??"":"";
    static double? Num(JsonElement e,string name)=>e.TryGetProperty(name,out var v)&&v.ValueKind==JsonValueKind.Number&&v.TryGetDouble(out double n)?n:null;
    static bool? Bool(JsonElement e,string name)=>e.TryGetProperty(name,out var v)&&v.ValueKind is JsonValueKind.True or JsonValueKind.False?v.GetBoolean():null;
    static float Clamp01(float v)=>Math.Clamp(v,0,1);
    static float Up(float t,float delay,float dur=.8f)=>ArtworkTheme.Ease(Clamp01((t-delay)/dur));
    Color A(Color c)=>ArtworkTheme.Fade(c,_alpha);
    static Color White(int alpha)=>Color.FromArgb(Math.Clamp(alpha,0,255),255,255,255);
    float Hov(string id,bool on) {if(on)_hovOn.Add(id);if(!_hov.TryGetValue(id,out float value)){if(!on)return 0;_hov[id]=0;return 0;}return value;}
    float Press(string id)=>_pressed==id?1:0;
    Font FD(float px)=>Fnt(0,px);
    Font FB(float px,bool bold=false)=>Fnt(bold?2:1,px);
    Font FM(float px,bool bold=false)=>Fnt(bold?4:3,px);
    float Advance(Graphics g,string text,float px)=>g.MeasureString(text,FD(px),100000,ArtworkTheme.Typo).Width;
    void AddHit(string id,float x,float y,float w,float h)=>_hits.Add((id,new(x,y,w,h)));
    void Hit(string id,float x,float y,float w,float h)=>AddHit(id,x,y,w,h);
    static Color Blend(Color c)=>Color.FromArgb(255,(c.R*3+245)/4,(c.G*3+245)/4,(c.B*3+245)/4);
    string RendererChip()=>Project?.Platform switch { DesktopPlatform.MacOS=>"METAL RENDERER",DesktopPlatform.Linux=>"VULKAN / DXVK",_=>_iniValues.GetValueOrDefault("renderer","auto") switch {"vulkan"=>"VULKAN RENDERER","direct3d11"=>"D3D11 RENDERER",_=>"AUTO RENDERER"} };
}
