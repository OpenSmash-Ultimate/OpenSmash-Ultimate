using System.Reflection;
using Avalonia.Threading;
using OpenSmash;
using OpenSmash.Core;

static class UpstreamChecks
{
    static void Check(bool ok,string message){if(!ok)throw new Exception(message);Console.WriteLine("PASS "+message);}
    static void Finish(Task task){var until=DateTime.UtcNow.AddSeconds(20);while(!task.IsCompleted){Dispatcher.UIThread.RunJobs();if(DateTime.UtcNow>until)throw new TimeoutException();Thread.Sleep(10);}task.GetAwaiter().GetResult();}
    public static void Run(string temporary)
    {
        var fit=typeof(MainWindow).GetMethod("FitToArea",BindingFlags.Static|BindingFlags.NonPublic)!;
        Avalonia.PixelRect Fit(Avalonia.PixelRect w,Avalonia.PixelRect a)=>(Avalonia.PixelRect)fit.Invoke(null,[w,a])!;
        var second=new Avalonia.PixelRect(1920,0,1280,720);
        var grown=Fit(new Avalonia.PixelRect(2000,40,2160,1215),second);
        Check(grown.Width<=1280&&grown.Height<=720&&second.Contains(grown)&&Math.Abs(grown.Width*9-grown.Height*16)<=16,"a window larger than the monitor it moved to is fitted inside it at 16:9");
        var fits=new Avalonia.PixelRect(2000,40,960,540);
        Check(Fit(fits,second)==fits,"a window that fits its monitor keeps its size and place");
        string root=Path.Combine(temporary,"upstream-regressions");Directory.CreateDirectory(Path.Combine(root,"tools"));Directory.CreateDirectory(Path.Combine(root,"runtime"));File.WriteAllText(Path.Combine(root,"tools","os_mods.py"),"");
        var project=new Project(root,Project.CurrentPlatform);Directory.CreateDirectory(Path.GetDirectoryName(project.GameExecutable)!);File.WriteAllText(project.GameExecutable,"synthetic fixture");
        File.WriteAllText(project.ConfigFile("display.ini"),"renderer = auto\nwindow = windowed\nvolume = 100\n");
        string defaultsRoot=Path.Combine(temporary,"display-defaults");Directory.CreateDirectory(Path.Combine(defaultsRoot,"tools"));Directory.CreateDirectory(Path.Combine(defaultsRoot,"runtime"));File.WriteAllText(Path.Combine(defaultsRoot,"tools","os_mods.py"),"");
        var defaults=new MainWindow([],defaultsRoot,enableBackgroundActions:false);
        Check(defaults.Surface.GetSetting("window")=="borderless"&&defaults.Surface.GetSetting("window_size")=="1920x1080"&&defaults.Surface.GetSetting("pacing")=="display"&&defaults.Surface.GetSetting("render_scale")=="1"&&defaults.Surface.GetSetting("gpu")=="auto","fresh launcher uses the requested portable display and graphics defaults");
        if(OperatingSystem.IsWindows())Check(defaults.Surface.GetSetting("renderer")=="vulkan","fresh Windows launcher defaults to Vulkan");
        defaults.Close();
        var window=new MainWindow([],root,enableBackgroundActions:false);var surface=window.Surface;
        var settingDefinitions=(Array)typeof(OpenSmash.Visuals.ClassicSurface).GetField("_settings",BindingFlags.Instance|BindingFlags.NonPublic)!.GetValue(surface)!;
        int shaderIndex=Array.FindIndex(settingDefinitions.Cast<object>().ToArray(),s=>(string)s.GetType().GetField("Key")!.GetValue(s)! == "preload_shaders");
        object shaderSetting=settingDefinitions.GetValue(shaderIndex)!;
        Check(((string[])shaderSetting.GetType().GetField("Values")!.GetValue(shaderSetting)!).Length==0,"Preload all shaders is an action without dropdown values");
        string? shaderAction=null;surface.ActionRequested+=id=>shaderAction=id;
        surface.Activate("set:"+shaderIndex);Check(shaderAction=="settings:shaders","Display shader button requests the installer's shader compilation");
        File.WriteAllText(project.PathIn("tools","shader_aot.py"),"import pathlib,sys,time\np=pathlib.Path('.')\nwith (p/'shader-runs.txt').open('a') as f:f.write(' '.join(sys.argv[1:])+'\\n')\nprint('42 of 100 shaders, about a minute left',flush=True)\nwhile not (p/'shader-release').exists():time.sleep(.05)\nprint('100 of 100 shaders, done',flush=True)\n");
        var compile=typeof(MainWindow).GetMethod("CompileShaders",BindingFlags.Instance|BindingFlags.NonPublic)!;
        var active=(bool?)false;
        void WaitForProgress(){var until=DateTime.UtcNow.AddSeconds(15);while((float)typeof(OpenSmash.Visuals.ClassicSurface).GetField("_shaderFrac",BindingFlags.Instance|BindingFlags.NonPublic)!.GetValue(surface)!<.42f){Dispatcher.UIThread.RunJobs();if(DateTime.UtcNow>until)throw new TimeoutException("Shader progress fixture did not run");Thread.Sleep(10);}}
        var shaderTask=(Task)compile.Invoke(window,["medium"])!;WaitForProgress();
        Check(((Task)compile.Invoke(window,["medium"])!).IsCompleted&&File.ReadAllLines(project.PathIn("shader-runs.txt")).Length==1,"repeated starts do not launch a second shader compiler");
        File.WriteAllText(project.PathIn("shader-release"),"");Finish(shaderTask);
        active=(bool)typeof(OpenSmash.Visuals.ClassicSurface).GetField("_shaderUi",BindingFlags.Instance|BindingFlags.NonPublic)!.GetValue(surface)!;
        Check(active==false&&File.ReadAllText(project.PathIn("shader-runs.txt")).Contains("--priority medium"),"shader action uses the existing compiler and clears progress on success");
        File.Delete(project.PathIn("shader-release"));shaderTask=(Task)compile.Invoke(window,["medium"])!;WaitForProgress();
        surface.Activate("set:"+shaderIndex);Check(shaderAction=="shaders:cancel","running shader button offers cancellation");
        Finish((Task)typeof(MainWindow).GetMethod("Action",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(window,["shaders:cancel"])!);Finish(shaderTask);
        Check(!(bool)typeof(OpenSmash.Visuals.ClassicSurface).GetField("_shaderUi",BindingFlags.Instance|BindingFlags.NonPublic)!.GetValue(surface)!&&!File.ReadAllText(project.ConfigFile("display.ini")).Contains("preload_shaders"),"cancel stops compilation without saving a shader toggle");
        Check(surface.GetSetting("hot_reload_mod_assets")=="0","mod asset hot reload is disabled without a saved preference");
        surface.ChangeSetting("hot_reload_mod_assets","1");
        var reopened=new MainWindow([],root,enableBackgroundActions:false);
        Check(reopened.Surface.GetSetting("hot_reload_mod_assets")=="1","enabled developer preference survives reopening the launcher");reopened.Close();
        surface.ChangeSetting("volume","75");
        Check(File.ReadAllText(project.ConfigFile("launcher.ini")).Contains("hot_reload_mod_assets = 1"),"unrelated settings preserve the developer preference");
        surface.ChangeSetting("hot_reload_mod_assets","0");
        reopened=new MainWindow([],root,enableBackgroundActions:false);
        Check(reopened.Surface.GetSetting("hot_reload_mod_assets")=="0","disabled developer preference survives reopening the launcher");reopened.Close();
        File.WriteAllText(project.ConfigFile("display.ini"),"renderer = vulkan\nwindow = fullscreen\nvolume = 100\n");
        var definitions=(Array)typeof(OpenSmash.Visuals.ClassicSurface).GetField("_settings",BindingFlags.Instance|BindingFlags.NonPublic)!.GetValue(surface)!;
        var developer=definitions.Cast<object>().Single(s=>(string)s.GetType().GetField("Key")!.GetValue(s)! == "hot_reload_mod_assets");
        Check(((string[])developer.GetType().GetField("Labels")!.GetValue(developer)!).SequenceEqual(new[]{"Disabled","Enabled"}),"developer dropdown offers Disabled and Enabled");
        var renderer=definitions.Cast<object>().Single(s=>(string)s.GetType().GetField("Key")!.GetValue(s)! == "renderer");
        string[] labels=(string[])renderer.GetType().GetField("Labels")!.GetValue(renderer)!;
        Check(labels[0].Contains("NVIDIA")&&labels[0].Contains("Intel"),"automatic renderer label names all supported GPU vendors");
        surface.ChangeSetting("volume","22");surface.ValidateSettingsForPlay();
        string saved=File.ReadAllText(project.ConfigFile("display.ini"));Check(saved.Contains("renderer = vulkan")&&saved.Contains("window = fullscreen")&&saved.Contains("volume = 22"),"settings writes preserve external/game renderer and window changes");
        surface.ChangeSetting("renderer","auto");Check(File.ReadAllText(project.ConfigFile("display.ini")).Contains("renderer = auto"),"explicitly reselecting the cached renderer still saves the user's choice");
        Directory.CreateDirectory(project.PathIn("dump","shadercache"));File.WriteAllText(project.PathIn("runtime","gpu_shader_cache.h"),"#define SHADER_CACHE_VERSION 9");
        File.WriteAllText(project.PathIn("build","ssbu_glslang.dll"),"fixture");string marker=project.PathIn("dump","shadercache","complete.txt");File.WriteAllText(marker,"version 9\n");
        surface.ChangeSetting("renderer","auto");bool Done()=>(bool)typeof(MainWindow).GetMethod("ShadersDone",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(window,null)!;
        Check(!Done(),"Vulkan-capable shader cache is unfinished until SPIR-V is recorded");Check(surface.ShadersPending(),"preload button remains available for unfinished shaders");File.AppendAllText(marker,"spirv fixture\n");Check(Done(),"complete SPIR-V marker satisfies shader readiness");Check(!surface.ShadersPending(),"preload button disappears when shaders are complete");File.WriteAllText(marker,"version 9\n");surface.ChangeSetting("renderer","direct3d11");Check(Done(),"explicit Direct3D does not demand a SPIR-V cache");
        Directory.CreateDirectory(project.PathIn("OpenSmash.Launcher","bin"));File.WriteAllText(project.PathIn("OpenSmash.Launcher","source.cs"),"source");string fingerprint=UpdateState.LauncherFingerprint(project);File.WriteAllText(project.PathIn("OpenSmash.Launcher","bin","generated.dll"),"ignored");Check(UpdateState.LauncherFingerprint(project)==fingerprint,"launcher update fingerprint ignores build outputs");File.AppendAllText(project.PathIn("OpenSmash.Launcher","source.cs"),"changed");Check(UpdateState.LauncherFingerprint(project)!=fingerprint,"launcher update fingerprint detects source changes");
        if(OperatingSystem.IsWindows())
        {
            File.WriteAllText(project.PathIn("tools","install_windowsmash.ps1"),"param([switch]$BuildAfterUpdate)\nif(-not $BuildAfterUpdate -or $env:SSBU_GUI_INSTALL -ne '1'){exit 3}\nif(Test-Path -LiteralPath (Join-Path $PSScriptRoot 'fail')){exit 7}\nexit 0\n");
            var method=typeof(MainWindow).GetMethod("FinishPendingGameUpdate",BindingFlags.Instance|BindingFlags.NonPublic)!;
            UpdateState.Mark(UpdateState.GameMarker(project),"fixture");File.WriteAllText(project.PathIn("tools","fail"),"");bool failed=false;
            try{Finish((Task)method.Invoke(window,null)!);}catch(InvalidOperationException){failed=true;}
            Check(failed&&File.Exists(UpdateState.GameMarker(project)),"failed update rebuild keeps its pending marker for retry");
            File.Delete(project.PathIn("tools","fail"));Finish((Task)method.Invoke(window,null)!);Check(!File.Exists(UpdateState.GameMarker(project)),"successful retry clears pending game update");
        }
        var windowsProject=new Project(root,DesktopPlatform.Windows);
        string configured=project.PathIn("configured-python.exe"),own=project.PathIn("deps","venv","Scripts","python.exe");
        Directory.CreateDirectory(Path.GetDirectoryName(own)!);File.WriteAllText(configured,"fixture");File.WriteAllText(own,"fixture");File.WriteAllText(project.PathIn("build","python_path.txt"),configured);
        Check(windowsProject.Python==configured,"an unfinished venv does not displace configured tools Python");
        File.WriteAllText(project.PathIn("deps","venv",".opensmash-ready"),"");Check(windowsProject.Python==own,"ready project-owned Python takes precedence over old configured Python");
        window.Close();
        string fresh=Path.Combine(root,"fresh-install");Directory.CreateDirectory(Path.Combine(fresh,"tools"));Directory.CreateDirectory(Path.Combine(fresh,"runtime"));File.WriteAllText(Path.Combine(fresh,"tools","os_mods.py"),"");
        var first=new MainWindow([],fresh,enableBackgroundActions:false);var installed=new Project(fresh,Project.CurrentPlatform);
        File.WriteAllText(installed.ConfigFile("online_name.txt"),"Setup player");Directory.CreateDirectory(Path.GetDirectoryName(installed.GameExecutable)!);File.WriteAllText(installed.GameExecutable,"fixture");first.Surface.SetProject(installed);
        Check(first.Surface.GetSetting("name")=="Setup player","finishing setup reloads the new game settings before opening the launcher");first.Close();
    }
}
