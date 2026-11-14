using System.Net.Http;
using System.Text;
using System.Text.Json;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using OpenSmash.Core;
using OpenSmash.Visuals;

namespace OpenSmash;

public sealed partial class MainWindow : Window
{
    public ClassicSurface Surface { get; } = new();
    Project? _project;
    CancellationTokenSource? _installerCancel;
    StreamWriter? _installerInput;
    TaskCompletionSource<string>? _shaderPriority;
    bool _closeAfterInstall;
    readonly bool _backgroundActions;
    readonly SemaphoreSlim _modStatusGate=new(1,1);
    static readonly HttpClient Http=new() {Timeout=TimeSpan.FromSeconds(20)};

    public MainWindow(string[] args,string? launcherDirectory=null,bool enableBackgroundActions=true)
    {
        _backgroundActions=enableBackgroundActions;
        Title="OpenSmash";WindowDecorations=Avalonia.Controls.WindowDecorations.None;CanResize=true;MinWidth=640;MinHeight=360;
        Background=Avalonia.Media.Brush.Parse("#050505");Width=1440;Height=810;
        WindowStartupLocation=WindowStartupLocation.CenterScreen;Content=Surface;
        Surface.ShadersPending=()=>_project?.IsBuilt==true&&!ShadersDone();
        using(var stream=typeof(MainWindow).Assembly.GetManifestResourceStream("opensmash.ico")) if(stream!=null)Icon=new WindowIcon(stream);
        if(enableBackgroundActions)Surface.ActionRequested+=async id=>await Action(id);
        Surface.DragRequested+=e=>
        {
            // Windows runs the move loop inside BeginMoveDrag: fit once on release, not while crossing monitors.
            _moving=OperatingSystem.IsWindows();try{BeginMoveDrag(e);}finally{if(_moving){_moving=false;FitToScreen();}}
        };
        Surface.ResizeRequested+=(edge,e)=>BeginResizeDrag(edge,e);
        ScalingChanged+=(_,_)=>{if(!_moving)Dispatcher.UIThread.Post(FitToScreen);};
        if(OperatingSystem.IsWindows())Win32Properties.AddWndProcHookCallback(this,KeepAspectWhileSizing);
        else Resized+=(_,e)=>{if(e.Reason==WindowResizeReason.User)Dispatcher.UIThread.Post(KeepAspect);};
        if(enableBackgroundActions)Surface.MarketplaceMessage+=async message=>await MarketplaceMessage(message);
        Surface.InputRequested+=answer=> {if(_shaderPriority!=null){_shaderPriority.TrySetResult(answer);return;}try{_installerInput?.WriteLine(answer);}catch(IOException error){Surface.Alert("INPUT FAILED",error.Message);} };
        Closing+=(_,e)=>
        {
            if(Surface.GameRunning&&!Surface.StartingGame){e.Cancel=true;Surface.Alert("CLOSE THE GAME","Close the game before exiting the launcher.");}
            else if(_bootstrapCancel!=null){e.Cancel=true;Surface.Confirm("CANCEL THE DOWNLOAD?","The project will be left untouched. You can download again later.",()=>{_closeAfterBootstrap=true;_bootstrapCancel?.Cancel();});}
            else if(Surface.IsInstalling){e.Cancel=true;Surface.Confirm("STOP THE BUILD?","Nothing that finished is lost. The next start goes on from where it stopped.",()=>{_closeAfterInstall=true;_installerCancel?.Cancel();});}
            else if(Surface.OperationRunning){e.Cancel=true;Surface.Alert("WORKING","Wait for the current mod operation to finish before closing.");}
        };
        Opened+=(_,_)=>
        {
            Surface.StartEntrance();
            _opened=true;
            if(enableBackgroundActions)_=CheckExternalChanges(atBoot:true);
            Surface.Focus();
            if(Screens.Primary is { } screen)
            {
                int h=Math.Min((int)(screen.WorkingArea.Height*.88),(int)(810*screen.Scaling));int w=(int)(h*16d/9);
                if(w>screen.WorkingArea.Width*.94){w=(int)(screen.WorkingArea.Width*.94);h=(int)(w*9d/16);}
                Width=w/screen.Scaling;Height=h/screen.Scaling;
                Position=new PixelPoint(screen.WorkingArea.X+(screen.WorkingArea.Width-w)/2,screen.WorkingArea.Y+(screen.WorkingArea.Height-h)/2);
            }
        };
        Closed+=(_,_)=>{_closed=true;Surface.CancelChangePrompt();_shaderCancel?.Cancel();};
        if(enableBackgroundActions)Activated+=(_,_)=>{if(_opened)_=CheckExternalChanges();};
        string? root=null;int at=Array.IndexOf(args,"--project");if(at>=0&&at+1<args.Length)root=args[at+1];
        try
        {
            if(args.Contains("--first-run"))root=null;
            else root??=Project.AtLauncherLocation(launcherDirectory??AppContext.BaseDirectory);
            if(root!=null)SetProject(root,args.Contains("--installer"));
        }
        catch(Exception error){Surface.Alert("COULD NOT OPEN PROJECT",error.Message);}
        if(_project==null)Surface.ShowFirstRun(DefaultBootstrapParent());
        if(args.Contains("--play")&&_project?.IsBuilt==true)Opened+=async(_,_)=>await Play();
    }

    bool _moving;
    // A monitor with other scaling keeps the size in DIPs, so the window can grow past that screen.
    void FitToScreen()
    {
        if(_closed||WindowState!=WindowState.Normal||Screens.ScreenFromWindow(this) is not { } screen)return;
        double scale=RenderScaling;var now=new PixelRect(Position,PixelSize.FromSize(ClientSize,scale));
        var fit=FitToArea(now,screen.WorkingArea);if(fit==now)return;
        Width=fit.Width/scale;Height=fit.Height/scale;Position=fit.Position;
    }
    internal static PixelRect FitToArea(PixelRect window,PixelRect area)
    {
        if(window.Width<=area.Width&&window.Height<=area.Height)return window;
        int h=(int)(area.Height*.88),w=(int)(h*16d/9);
        if(w>area.Width*.94){w=(int)(area.Width*.94);h=(int)(w*9d/16);}
        int x=Math.Clamp(window.X+(window.Width-w)/2,area.X,area.Right-w),y=Math.Clamp(window.Y+(window.Height-h)/2,area.Y,area.Bottom-h);
        return new PixelRect(x,y,w,h);
    }
    void KeepAspect()
    {
        if(_closed||WindowState!=WindowState.Normal)return;
        double h=Math.Round(ClientSize.Width*9/16);if(Math.Abs(h-ClientSize.Height)>1)Height=h;
    }
    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    struct Win32Rect{public int Left,Top,Right,Bottom;}
    [System.Runtime.InteropServices.DllImport("user32.dll")]static extern bool GetWindowRect(IntPtr hwnd,out Win32Rect rect);
    [System.Runtime.InteropServices.DllImport("user32.dll")]static extern bool GetClientRect(IntPtr hwnd,out Win32Rect rect);
    IntPtr KeepAspectWhileSizing(IntPtr hwnd,uint message,IntPtr edge,IntPtr rectPointer,ref bool handled)
    {
        const uint WM_SIZING=0x0214;if(message!=WM_SIZING||!GetWindowRect(hwnd,out var outer)||!GetClientRect(hwnd,out var inner))return IntPtr.Zero;
        var rect=System.Runtime.InteropServices.Marshal.PtrToStructure<Win32Rect>(rectPointer);
        int frameW=outer.Right-outer.Left-inner.Right,frameH=outer.Bottom-outer.Top-inner.Bottom;
        rect=SizeWithAspect(rect,(int)edge,frameW,frameH);
        System.Runtime.InteropServices.Marshal.StructureToPtr(rect,rectPointer,false);handled=true;return new IntPtr(1);
    }
    // WMSZ_ edges: 1 left, 2 right, 3 top, 4 top-left, 5 top-right, 6 bottom, 7 bottom-left, 8 bottom-right.
    static Win32Rect SizeWithAspect(Win32Rect r,int edge,int frameW,int frameH)
    {
        int w=r.Right-r.Left-frameW,h=r.Bottom-r.Top-frameH;
        if(edge is 3 or 6)w=(int)Math.Round(h*16d/9);else h=(int)Math.Round(w*9d/16);
        if(edge is 1 or 4 or 7)r.Left=r.Right-w-frameW;else r.Right=r.Left+w+frameW;
        if(edge is 3 or 4 or 5)r.Top=r.Bottom-h-frameH;else r.Bottom=r.Top+h+frameH;
        return r;
    }

    void SetProject(string root,bool forceInstaller=false)
    {
        _project=new Project(root,Project.CurrentPlatform);ProjectLayout.Normalize(_project.Root);Surface.SetProject(_project,forceInstaller);
        if(_opened&&_backgroundActions)_=CheckExternalChanges(atBoot:true);
    }
    Project RequireProject()=>_project??throw new InvalidOperationException("Choose an OpenSmash project folder first.");
    async Task ChooseProject()
    {
        var picked=await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions{Title="Choose OpenSmash project",AllowMultiple=false});
        if(picked.FirstOrDefault()?.TryGetLocalPath() is string folder)SetProject(folder);
    }
    async Task Action(string id)
    {
        try
        {
            if(_checkingChanges&&id=="play"){_playQueued=true;return;}
            if((_checkingChanges||_playPreflight)&&id is not ("close" or "min"))return;
            switch(id)
            {
                case "close":Close();break;
                case "min":WindowState=WindowState.Minimized;break;
                case "project:ready":await CheckExternalChanges(atBoot:true);break;
                case "project:choose":await ChooseProject();break;
                case "bootstrap:browse":await ChooseBootstrapParent();break;
                case "bootstrap:download":await DownloadProject();break;
                case "bootstrap:cancel":_bootstrapCancel?.Cancel();break;
                case "play":await Play();break;
                case "settings:shaders":await CompileShaders("medium");break;
                case "shaders:cancel":_shaderCancel?.Cancel();break;
                case "pick:game":case "pick:keys":case "pick:firmware":
                    var folders=await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions{Title="Choose "+id[5..]+" folder",AllowMultiple=false});
                    if(folders.FirstOrDefault()?.TryGetLocalPath() is string folder)Surface.SetFolder(id[5..],folder);break;
                case "clear:firmware":Surface.SetFolder("firmware","");break;
                case "act:check" when Surface.IsInstaller:await CheckFiles();break;
                case "act:install":case "act:resume":await Install();break;
                case "act:stop":Surface.Confirm("STOP THE BUILD?","Nothing that finished is lost. You can resume from this screen.",()=>_installerCancel?.Cancel());break;
                case "act:logs":
                    string log=RequireProject().PathIn("build","logs");Directory.CreateDirectory(log);Commands.OpenPath(log);break;
                case "mods:refresh":case "act:refresh":await CheckExternalChanges(force:true);break;
                case "act:open":Commands.OpenPath(RequireProject().PathIn("mods"));break;
                case "act:setup":Surface.Navigate("setup_modding");await RefreshMods();break;
                case "act:browse":Surface.Navigate("mods_browser");Surface.EnsureMarketplace();break;
                case "act:check":await CheckMods();break;
                case "act:share":Surface.ShowSharePrompt();break;
                case "mods:export":await ShareModList(true);break;
                case "mods:import":await ShareModList(false);break;
                case "mods:update":foreach(var update in Surface.ModUpdates)await ModCommand("install",[update.gb,update.file]);break;
                case "act:add":await AddLocalMod();break;
                case "setup_start":case "setup_again":
                    Surface.PluginActivity(true);
                    try{await ModCommand("fetch",id=="setup_again"?["--again"]:[]);Surface.PluginActivity(false);}
                    catch{Surface.PluginActivity(false,failed:true);throw;}break;
                case "setup_add":await AddPluginFiles();break;
                case "act:remove":
                    string[] names=Surface.SelectedModNames;if(names.Length>0)Surface.Confirm("REMOVE MODS?","Remove "+string.Join(", ",names)+" from this project?",async()=>{try{await ModCommand("remove",names);}catch(Exception error){Surface.Alert("COULD NOT REMOVE",error.Message);}});break;
                case "act:build":await ModCommand("build");break;
                case "act:rebuild":
                    string rebuild=await Surface.ShowRebuildPrompt();
                    if(rebuild is "build" or "refresh")await ModCommand(rebuild);break;
                case "settings:pads":
                    string pads=RequireProject().ConfigFile("controllers.ini");if(!File.Exists(pads))File.WriteAllText(pads,"[general]\nkeyboard_player = 1\nbackground = 1\n");Commands.OpenPath(pads);break;
                case "update:check":await CheckUpdates();break;
                case "update:apply":await ApplyUpdate();break;
                case "storage:refresh":Surface.SetStorageResult(await Python("data_location.py"));break;
                case "storage:move":
                    var targets=await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions{Title="Choose a destination for dump and build",AllowMultiple=false});
                    if(targets.FirstOrDefault()?.TryGetLocalPath() is string target)Surface.Confirm("MOVE GAME FILES?","Move dump and build into "+target+"? Close the game first. This can take a while; links are left in their place.",async()=>{try{Surface.SetStorageResult(await Python("data_location.py","--move",target));}catch(Exception error){Surface.Alert("MOVE STOPPED",error.Message);}});break;
                default:
                    if(id.StartsWith("modtoggle:")&&int.TryParse(id[10..],out int row))await ModCommand(Surface.ModEnabled(row)?"disable":"enable",Surface.ModNames(row));
                    else if(id.StartsWith("modvariant:"))await ModCommand("enable",[id[11..]]);
                    else if(id.StartsWith("modrow:"))Surface.SelectMod(id);
                    break;
            }
        }
        catch(Exception error){Surface.Alert("OPENSMASH",error.Message);}
    }

    async Task CheckFiles()
    {
        if(!Surface.CheckFolders()){Surface.Alert("FIX THESE FOLDERS","Choose your game files and keys. Firmware can be left empty.");return;}
        await EnsureToolsPython();
        var p=RequireProject();var output=new StringBuilder();
        await ProcessRunner.Run(new Command(p.Python,[p.PathIn("tools","gamefiles.py"),Surface.GameFolder,"--keys",Surface.KeysFolder,"--json"],p.Root),line=>output.AppendLine(line),CancellationToken.None);
        string json=output.ToString();
        using var doc=JsonDocument.Parse(json);Surface.ShowFileCheck(doc.RootElement);
    }
    async Task Install()
    {
        if(Surface.IsInstalling)return;
        var p=RequireProject();
        if(!Surface.CheckFolders()){Surface.Alert("FIX THESE FOLDERS","Choose your game files and keys. The line under each folder says what is missing.");return;}
        if(Surface.Username==""){Surface.Alert("TYPE A USERNAME","Type a username before the build can start.");return;}
        if(p.Platform==DesktopPlatform.Windows&&!Surface.AcceptLicense){Surface.Alert("LICENSE TERMS","Accept Microsoft's license terms in Options before installing the compiler, or install the build tools yourself first.");return;}
        var options=new Commands.InstallOptions(Surface.GameFolder,Surface.KeysFolder,Surface.FirmwareFolder,Surface.Username,Surface.Jobs,
            Surface.Shortcuts,Surface.Firewall,Surface.Shaders,Surface.Updates,Surface.AcceptLicense);
        Directory.CreateDirectory(p.PathIn("build","logs"));
        Surface.BeginInstall();_installerCancel=new CancellationTokenSource();int exit=1;
        try
        {
            exit=await ProcessRunner.Run(Commands.Install(p,options),line=>Dispatcher.UIThread.Post(()=>Surface.ReceiveInstallLine(line)),_installerCancel.Token,
                logPath:p.PathIn("build","logs","installer_window.log"),inputReady:writer=>_installerInput=writer,
                environment:Commands.GuiEnvironment(p,options.Shortcuts,options.Firewall));
            if(exit==0&&p.IsBuilt&&options.Shaders&&p.Platform!=DesktopPlatform.Windows)
            {
                _installerInput=null;_shaderPriority=new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);
                Surface.ReceiveInstallLine("SHADER_PRIORITY?");string priority=await _shaderPriority.Task.WaitAsync(_installerCancel.Token);_shaderPriority=null;
                Surface.BeginShaderStep();
                exit=await ProcessRunner.Run(new Command(p.Python,[p.PathIn("tools","shader_aot.py"),"--priority",priority],p.Root),
                    line=>Dispatcher.UIThread.Post(()=>Surface.ReceiveInstallLine(line)),_installerCancel.Token,logPath:p.PathIn("build","logs","shader_window.log"));
            }
        }
        catch(OperationCanceledException){exit=130;Surface.ReceiveInstallLine("Build stopped. Completed work is kept; press Resume to continue.");}
        catch(Exception error){Surface.ReceiveInstallLine("ERROR: "+error.Message);}
        finally{_shaderPriority=null;_installerInput=null;_installerCancel.Dispose();_installerCancel=null;Surface.EndInstall(exit);Surface.SetProject(p);if(_closeAfterInstall)Close();}
    }
    async Task Play()
    {
        var p=RequireProject();if(!p.IsBuilt||Surface.GameRunning||Surface.IsInstalling||Surface.ModsRunning||Surface.OperationRunning)return;
        if(_playPreflight||_checkingChanges||Surface.HasPrompt)return;
        _playPreflight=true;
        try
        {
            if(!await PrepareExternalChanges(forPlay:true))return;
            if(!Surface.ValidateSettingsForPlay())return;
        }
        finally{_playPreflight=false;}
        if(_closed)return;
        Surface.GameRunning=true;Surface.StartingGame=true;Surface.InvalidateVisual();
        try
        {
            await Task.Delay(150); // Give the starting state its first frame before preparing mods.
            if(Directory.Exists(p.PathIn("mods"))&&File.Exists(p.PathIn("mods","mods.json")))await ModCommand(Surface.LaunchMode=="vanilla"?"vanilla":"apply",refresh:false);
            Directory.CreateDirectory(p.PathIn("build"));
            await GameLaunch.StartAndWait(Commands.Play(p,true),p.Platform,p.PathIn("build","opensmash-play.log"),
                nativeLog:p.Platform==DesktopPlatform.MacOS?p.PathIn("build","play.log"):null);
            Surface.GameRunning=false;Surface.StartingGame=false;Close();
        }
        finally{Surface.GameRunning=false;Surface.StartingGame=false;Surface.InvalidateVisual();}
    }
    Task<string> Python(string script,params string[] args)=>Python(script,args,null);
    async Task<string> Python(string script,string[] args,Action<string>? observe)
    {
        var p=RequireProject();var output=new StringBuilder();
        int exit=await ProcessRunner.Run(new Command(p.Python,new[]{p.PathIn("tools",script)}.Concat(args).ToArray(),p.Root),line=>{output.AppendLine(line);observe?.Invoke(line);},CancellationToken.None);
        if(exit!=0)throw new InvalidOperationException(output.Length>0?output.ToString():$"{script} failed (exit {exit}).");
        return output.ToString().Trim();
    }
    async Task RefreshMods()
    {
        if(_project==null)return;
        var project=_project;
        await _modStatusGate.WaitAsync();
        try
        {
            if(project!=_project||_closed)return;
            string output=await Python("os_mods.py","status");
            string json=output.Split('\n').Last(l=>l.TrimStart().StartsWith('{'));
            using var document=JsonDocument.Parse(json);
            if(document.RootElement.TryGetProperty("error",out var error))throw new InvalidOperationException(error.GetString());
            if(project==_project&&!_closed)Surface.SetMods(json);
        }
        finally{_modStatusGate.Release();}
    }
    async Task ModCommand(string command,string[]? names=null,bool refresh=true)
    {
        if(Surface.ModsRunning)throw new InvalidOperationException("Close the mod manager before changing mods here.");
        if(Surface.OperationRunning)throw new InvalidOperationException("Wait for the current mod operation to finish.");
        Surface.SetOperationLabel(command switch {"fetch"=>"DOWNLOADING PLUGINS","install"=>"INSTALLING MODS","remove"=>"REMOVING MODS","build"=>"UPDATING PLUGINS","refresh"=>"REFRESHING PLUGINS","apply"=>"PREPARING MODS","vanilla"=>"PREPARING VANILLA",_=>"UPDATING MODS"});
        _modMutationVersion++;
        Surface.OperationRunning=true;Surface.InvalidateVisual();
        bool pluginJob=command is "enable" or "disable" or "refresh" or "build";
        if(pluginJob)Surface.PluginBuildActivity(true);
        try
        {
        string result=await Python("os_mods.py",new[]{command}.Concat(names??[]).ToArray(),line=>{if(line.StartsWith("LOG "))Dispatcher.UIThread.Post(()=>{if(command=="fetch")Surface.PluginActivity(true,line[4..]);if(pluginJob&&Surface.OperationRunning)Surface.PluginBuildActivity(true,line[4..]);});});
        string? line=result.Split('\n').LastOrDefault(l=>l.StartsWith("RESULT "));
        if(line==null)throw new InvalidOperationException(result);
        using var document=JsonDocument.Parse(line[7..]);if(!document.RootElement.GetProperty("ok").GetBoolean())throw new InvalidOperationException(result);
        if(command=="enable"&&names!=null)new ModChanges(RequireProject()).Remember(names);
        if(command=="install"&&document.RootElement.TryGetProperty("names",out var added))
            new ModChanges(RequireProject()).Remember(added.EnumerateArray().Select(n=>n.GetString()!).Where(n=>n!=null));
        if(refresh)await RefreshMods();
        if(command is "enable" or "install")_dismissedPluginState=PluginPromptState();
        }
        finally{if(pluginJob)Surface.PluginBuildActivity(false);Surface.OperationRunning=false;Surface.InvalidateVisual();}
    }
}
