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
        string root=Path.Combine(temporary,"upstream-regressions");Directory.CreateDirectory(Path.Combine(root,"tools"));Directory.CreateDirectory(Path.Combine(root,"runtime"));File.WriteAllText(Path.Combine(root,"tools","os_mods.py"),"");
        var project=new Project(root,Project.CurrentPlatform);Directory.CreateDirectory(Path.GetDirectoryName(project.GameExecutable)!);File.WriteAllText(project.GameExecutable,"synthetic fixture");
        File.WriteAllText(project.ConfigFile("display.ini"),"renderer = auto\nwindow = windowed\nvolume = 100\n");
        var window=new MainWindow([],root,enableBackgroundActions:false);var surface=window.Surface;
        File.WriteAllText(project.ConfigFile("display.ini"),"renderer = vulkan\nwindow = fullscreen\nvolume = 100\n");
        var definitions=(Array)typeof(OpenSmash.Visuals.ClassicSurface).GetField("_settings",BindingFlags.Instance|BindingFlags.NonPublic)!.GetValue(surface)!;
        var renderer=definitions.Cast<object>().Single(s=>(string)s.GetType().GetField("Key")!.GetValue(s)! == "renderer");
        string[] labels=(string[])renderer.GetType().GetField("Labels")!.GetValue(renderer)!;
        Check(labels[0].Contains("NVIDIA")&&labels[0].Contains("Intel"),"automatic renderer label names all supported GPU vendors");
        surface.ChangeSetting("volume","22");surface.ValidateSettingsForPlay();
        string saved=File.ReadAllText(project.ConfigFile("display.ini"));Check(saved.Contains("renderer = vulkan")&&saved.Contains("window = fullscreen")&&saved.Contains("volume = 22"),"settings writes preserve external/game renderer and window changes");
        surface.ChangeSetting("renderer","auto");Check(File.ReadAllText(project.ConfigFile("display.ini")).Contains("renderer = auto"),"explicitly reselecting the cached renderer still saves the user's choice");
        Directory.CreateDirectory(project.PathIn("dump","shadercache"));File.WriteAllText(project.PathIn("runtime","gpu_shader_cache.h"),"#define SHADER_CACHE_VERSION 9");
        File.WriteAllText(project.PathIn("build","ssbu_glslang.dll"),"fixture");string marker=project.PathIn("dump","shadercache","complete.txt");File.WriteAllText(marker,"version 9\n");
        surface.ChangeSetting("renderer","auto");bool Done()=>(bool)typeof(MainWindow).GetMethod("ShadersDone",BindingFlags.Instance|BindingFlags.NonPublic)!.Invoke(window,null)!;
        Check(!Done(),"Vulkan-capable shader cache is unfinished until SPIR-V is recorded");File.AppendAllText(marker,"spirv fixture\n");Check(Done(),"complete SPIR-V marker satisfies shader readiness");File.WriteAllText(marker,"version 9\n");surface.ChangeSetting("renderer","direct3d11");Check(Done(),"explicit Direct3D does not demand a SPIR-V cache");
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
