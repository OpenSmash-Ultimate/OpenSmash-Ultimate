using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Interactivity;
using OpenSmash;
using OpenSmash.Core;

if (args.Contains("--process-fixture"))
{
    Console.WriteLine("stdout fixture");
    Console.Error.WriteLine("stderr fixture");
    Console.WriteLine(args[^1]);
    Environment.ExitCode = 7;
    return;
}
if(args.Contains("--input-fixture"))
{
    Console.WriteLine("PROMPT");Console.WriteLine(Console.ReadLine());Console.WriteLine(Environment.GetEnvironmentVariable("SSBU_GUI_INSTALL"));return;
}
if(args.Contains("--wait-fixture")){await Task.Delay(TimeSpan.FromSeconds(30));return;}
if(args.Length==3&&args[0]=="--game-launch-fixture")
{
    Console.WriteLine(args[2]);Console.WriteLine("RENDERER="+(Environment.GetEnvironmentVariable("SSBU_RENDERER")??"cleared"));Console.Error.WriteLine("[gfx] window ready (synthetic fixture)");
    var deadline=DateTime.UtcNow.AddSeconds(30);
    while(!File.Exists(args[1]+".release")&&DateTime.UtcNow<deadline)await Task.Delay(30);
    Console.WriteLine("after launcher handoff");File.WriteAllText(args[1],"survived");return;
}
if(args.Contains("--game-fail-fixture")){Console.Error.WriteLine("synthetic startup failure");Environment.ExitCode=9;return;}

if(args.Length==1&&args[0]=="--source-info"){Console.WriteLine(System.Text.Json.JsonSerializer.Serialize(BootstrapSource.Bundled()));return;}

if(args.Length==3&&args[0]=="--bootstrap-live")
{
    using var http=new System.Net.Http.HttpClient{Timeout=TimeSpan.FromMinutes(30)};
    var clock=System.Diagnostics.Stopwatch.StartNew();string phase="";
    var result=await new ProjectBootstrap(http).Install(args[1],BootstrapSource.Bundled(),new LauncherDeployment(Path.GetFullPath(args[2]),Project.CurrentPlatform),p=>
    {if(p.Phase!=phase||clock.ElapsedMilliseconds>1000){Console.WriteLine($"{p.Phase}: {p.Done} / {p.Total}");phase=p.Phase;clock.Restart();}},CancellationToken.None);
    Console.WriteLine("PASS live project download/extraction: "+result.ProjectFolder);
    Console.WriteLine("Installed launcher: "+result.Launcher);return;
}

static void Check(bool condition, string message)
{
    if (!condition) throw new Exception(message);
    Console.WriteLine("PASS " + message);
}

string root = Path.Combine(Path.GetTempPath(), "OpenSmash desktop test ' & space " + Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(Path.Combine(root, "runtime"));
Directory.CreateDirectory(Path.Combine(root, "tools"));
File.WriteAllText(Path.Combine(root, "tools", "os_mods.py"), "");
try
{
    foreach (var platform in Enum.GetValues<DesktopPlatform>())
    {
        var p = new Project(root, platform);
        Check(!p.IsBuilt, $"{platform}: absent game is not reported ready");
        var play = Commands.Play(p, true);
        var offline = Commands.Play(p, false);
        Check(play.WorkingDirectory == root, $"{platform}: launches from project root");
        Check(play.Arguments.SequenceEqual(play.StartInfo().ArgumentList), $"{platform}: arguments preserve spaces and shell characters");
        Check(!play.Arguments.SequenceEqual(offline.Arguments), $"{platform}: offline has a distinct command");
        Check(Commands.Mods(p).Arguments.Contains(root), $"{platform}: mod manager uses selected tree");
        Directory.CreateDirectory(Path.GetDirectoryName(p.GameExecutable)!);
        File.WriteAllText(p.GameExecutable, "synthetic test fixture");
        Check(p.IsBuilt, $"{platform}: detects correct platform build");
        File.Delete(p.GameExecutable);
    }
    ChangeChecks.Tracker(root);
    var project = new Project(root, Project.CurrentPlatform);
    Check(Project.AtLauncherLocation(root)==root,"launcher uses the project beside its executable");
    string empty=Path.Combine(root,"empty-launcher-folder");Directory.CreateDirectory(empty);
    Check(Project.AtLauncherLocation(empty)==null,"an empty folder inside another project does not inherit its parent project");
    string appBinary=Path.Combine(root,"OpenSmash.app","Contents","MacOS");Directory.CreateDirectory(appBinary);
    Check(Project.AtLauncherLocation(appBinary)==root,"macOS app resolves only the folder containing its bundle");
    Check(project.ConfigFile("display.ini") == Path.Combine(root, "display.ini"), "unmigrated configuration stays at root");
    Directory.CreateDirectory(Path.Combine(root, "config"));
    File.WriteAllText(Path.Combine(root, "config", "version.ini"), "1");
    Check(project.ConfigFile("display.ini") == Path.Combine(root, "config", "display.ini"), "migrated settings use config directory");
    Check(project.ConfigFile("online_server.txt") == Path.Combine(root, "online_server.txt"), "lobby fallback preserved during migration");
    File.WriteAllText(Path.Combine(root, "config", "online_server.txt"), "off");
    Check(project.ConfigFile("online_server.txt") == Path.Combine(root, "config", "online_server.txt"), "migrated lobby takes precedence");
    var lines = new List<string>();
    string marker = "argument with ' quotes & spaces";
    int exit = await ProcessRunner.Run(new Command("dotnet",
        [typeof(App).Assembly.Location.Replace("OpenSmash.dll", "OpenSmash.Tests.dll"), "--process-fixture", marker], root),
        lines.Add, CancellationToken.None, logPath: Path.Combine(root, "process.log"));
    Check(exit == 7, "process runner propagates nonzero exit code");
    Check(lines.Contains(marker) && lines.Contains("stderr fixture") && lines.Contains("stdout fixture"), "process runner drains both output streams and preserves arguments");
    Check(File.ReadAllText(Path.Combine(root, "process.log")).Contains(marker), "captured output is persisted to game log");
    string fixture=typeof(App).Assembly.Location.Replace("OpenSmash.dll","OpenSmash.Tests.dll");
    lines.Clear();
    int inputExit=await ProcessRunner.Run(new Command("dotnet",[fixture,"--input-fixture"],root),lines.Add,CancellationToken.None,
        inputReady:writer=>writer.WriteLine("high"),environment:new Dictionary<string,string>{{"SSBU_GUI_INSTALL","1"}});
    Check(inputExit==0&&lines.Contains("high")&&lines.Contains("1"),"installer input stays open and GUI environment reaches child processes");
    using(var cancel=new CancellationTokenSource(TimeSpan.FromMilliseconds(150)))
    {
        bool stopped=false;try{await ProcessRunner.Run(new Command("dotnet",[fixture,"--wait-fixture"],root),_=>{},cancel.Token);}catch(OperationCanceledException){stopped=true;}
        Check(stopped,"Stop cancels an installer process without waiting for completion");
    }
    foreach(var platform in Enum.GetValues<DesktopPlatform>())
    {
        var p=new Project(root,platform);var install=Commands.Install(p,new Commands.InstallOptions("game with ' & spaces","keys","","Player",2,Updates:false));
        Check(install.Arguments.Contains("game with ' & spaces")&&install.Arguments.Contains(platform==DesktopPlatform.Windows?"-NoWizard":"--yes"),$"{platform}: installer receives literal folder arguments without another wizard");
    }

    Check(BootstrapSource.Bundled().Revision.Length>=40,"launcher contains valid embedded project download information");
    await BootstrapChecks.Run(root,Check);
    string gameMarker=Path.Combine(root,"game-survived.txt"),gameLog=Path.Combine(root,"game %PATH% & log.txt");
    string ReadGameLog(){using var input=new FileStream(gameLog,FileMode.Open,FileAccess.Read,FileShare.ReadWrite|FileShare.Delete);using var reader=new StreamReader(input);return reader.ReadToEnd();}
    string literal="literal %PATH% & quotes \" and trailing slash\\";
    string? oldRenderer=Environment.GetEnvironmentVariable("SSBU_RENDERER");
    try{Environment.SetEnvironmentVariable("SSBU_RENDERER","direct3d11");await GameLaunch.StartAndWait(new Command("dotnet",[fixture,"--game-launch-fixture",gameMarker,literal],root),Project.CurrentPlatform,gameLog);}
    finally{Environment.SetEnvironmentVariable("SSBU_RENDERER",oldRenderer);}
    Check(ReadGameLog().Contains("RENDERER=cleared"),"game launch clears inherited renderer overrides so the saved renderer is used");
    Check(!File.Exists(gameMarker)&&ReadGameLog().Contains(literal),"game handoff waits for readiness and preserves literal Windows arguments");
    File.WriteAllText(gameMarker+".release","");
    for(int attempt=0;attempt<100&&!File.Exists(gameMarker);attempt++)await Task.Delay(50);
    Check(File.Exists(gameMarker)&&ReadGameLog().Contains("after launcher handoff"),"game and file logging survive disposal of the launcher's process handles");
    bool failedBeforeWindow=false;
    try{await GameLaunch.StartAndWait(new Command("dotnet",[fixture,"--game-fail-fixture"],root),Project.CurrentPlatform,Path.Combine(root,"failed-game.log"));}
    catch(InvalidOperationException error){failedBeforeWindow=error.Message.Contains("exit 9");}
    Check(failedBeforeWindow,"startup failure is reported instead of closing the launcher");

    AppBuilder.Configure<App>().WithInterFont().UseSkia().UseHeadless(new AvaloniaHeadlessPlatformOptions {
        UseHeadlessDrawing = false }).SetupWithoutStarting();
    var window = new MainWindow(["--project", root], root,enableBackgroundActions:false);
    window.Show(); window.Width=1440;window.Height=810;
    var surface=window.Surface;surface.FrozenTime=5;
    Check(surface.IsInstaller && surface.CurrentView=="installer:setup", "unbuilt projects open the original installer screen");
    string? screenshot = Environment.GetEnvironmentVariable("OPENSMASH_TEST_SCREENSHOT");
    void Capture(string name)
    {
        surface.InvalidateVisual();
        using var frame=window.CaptureRenderedFrame();
        Check(frame!=null&&frame.PixelSize.Width==1440, "renders original artwork: "+name);
        using(var png=new MemoryStream()){frame!.Save(png);ReferenceImages.Compare(name,png.ToArray());}
        if(screenshot!=null){Directory.CreateDirectory(Path.GetDirectoryName(screenshot)!);frame!.Save(Path.Combine(Path.GetDirectoryName(screenshot)!,name+".png"));}
    }
    Capture("installer-setup");
    surface.BeginInstall();
    string[] titles=["Check the tools","Install the Python packages","Your folders","Copy the game's program","The fonts: your firmware's","Connect your game data","Turn on your DLC","Take out the fighters","Build the game"];
    for(int n=0;n<titles.Length;n++)surface.ReceiveInstallLine($"[{n+1}/9] {titles[n]}");
    surface.ReceiveInstallLine("[-------------------------]  2% estimating the time left (writing the C code: synthetic_module, 54 of 101 modules)");
    Check(surface.CurrentView=="installer:build"&&Math.Abs(surface.Progress-54.0/101)<.001,"installer progress uses real module counts and original step parser");
    Capture("installer-build");
    surface.ReceiveInstallLine("##ssbu {\"ev\":\"plan\",\"steps\":[{\"id\":\"build\",\"title\":\"Build the game\",\"est_s\":100}]}");
    surface.ReceiveInstallLine("##ssbu {\"ev\":\"step\",\"id\":\"build\",\"state\":\"start\"}");
    surface.ReceiveInstallLine("##ssbu {\"ev\":\"progress\",\"id\":\"build\",\"done\":5,\"total\":10}");
    Check(surface.Progress==.5,"installer consumes structured progress events as well as plain output");
    surface.EndInstall(1);
    Check(surface.IsInstaller,"failed installation stays in installer for resume");
    Directory.CreateDirectory(Path.GetDirectoryName(project.GameExecutable)!);File.WriteAllText(project.GameExecutable,"synthetic fixture");
    surface.Navigate("setup");surface.SetProject(project);
    Check(!surface.IsInstaller&&surface.CurrentView=="home","built projects open the regular launcher");
    surface.FrozenTime=10;
    Capture("launcher-home");
    surface.GameRunning=true;surface.StartingGame=true;Capture("launcher-starting");
    Check(surface.StartingGame,"Play exposes the Starting game state while readiness is pending");
    surface.GameRunning=false;surface.StartingGame=false;
    surface.Navigate("settings");surface.FrozenTime=15;
    surface.ChangeSetting("volume","45");surface.ChangeSetting("window","borderless");
    Check(surface.GetSetting("volume")=="45"&&File.ReadAllText(project.ConfigFile("display.ini")).Contains("volume = 45"),"original settings controls persist game configuration");
    Capture("launcher-settings");
    foreach(var (tab,name) in new[]{(1,"graphics"),(2,"audio"),(3,"online"),(4,"controllers"),(5,"update"),(6,"storage")})
    {
        if(tab==6)surface.SetStorageResult("Synthetic storage fixture: game files stay on this computer.");
        surface.Activate("tab:"+tab);Capture("launcher-settings-"+name);
    }
    surface.Activate("tab:0");surface.Activate("set:0");surface.FrozenTime=16;
    var hover=(Dictionary<string,float>)surface.GetType().GetField("_hov",System.Reflection.BindingFlags.Instance|System.Reflection.BindingFlags.NonPublic)!.GetValue(surface)!;hover["set:0"]=1;hover["set:0:dd"]=1;
    Capture("launcher-settings-dropdown");surface.Activate("set:0");
    Check(OpenSmash.Visuals.ClassicSurface.MarketplaceHtml().Contains("invokeCSharpAction(JSON.stringify(value))"),"original marketplace page uses the portable host transport");
    surface.SetMods("{\"mods\":[{\"name\":\"example\",\"gamebanana\":\"123\",\"enabled\":false}]}");
    Check(surface.InstalledModIds.Contains("123"),"marketplace recognizes installed mods even when switched off");

    surface.FrozenTime=15;surface.Navigate("mods");surface.SetMods("{\"mods\":[]}");surface.FrozenTime=20;
    Capture("launcher-mods");
    surface.ShowSharePrompt();surface.FrozenTime=21;Capture("launcher-share");surface.Activate("share_prompt:cancel");
    surface.ShowModCheck("No file is changed by two turned-on mods.\n\nEvery GameBanana mod is up to date.",[]);surface.FrozenTime=22;Capture("launcher-check");surface.Activate("check_prompt:close");surface.FrozenTime=20;

    surface.Navigate("home");surface.FrozenTime=30;
    var prompt=surface.ShowNewMods(["Synthetic mod"]);surface.FrozenTime=31;Capture("launcher-new-mod");
    surface.Activate("nav:play");Check(!prompt.IsCompleted,"new-mod prompt blocks background actions");
    surface.Activate("newmods:build");Check(prompt.Result=="enable","original new-mod button enables the discovered mod");
    prompt=surface.ShowPluginPrompt(true,"Synthetic moveset has a new plugin file. Refreshing rebuilds its DLL. The game program stays as it is.");surface.FrozenTime=32;Capture("launcher-plugin-refresh");surface.Activate("plugin_prompt:build");Check(prompt.Result=="refresh","refresh prompt routes to DLL-only rebuild");
    prompt=surface.ShowPluginPrompt(false,"Synthetic plugin was added. The game has to be updated once so that plugin can run.");surface.FrozenTime=33;Capture("launcher-plugin-update");surface.Activate("plugin_prompt:build");Check(prompt.Result=="build","update prompt routes to game relink");
    prompt=surface.ShowRebuildPrompt();surface.FrozenTime=34;Capture("launcher-rebuild");surface.Activate("rebuild_prompt:refresh");Check(prompt.Result=="refresh","manual rebuild retains original refresh/update choice");
    prompt=surface.ShowShaderPrompt();surface.FrozenTime=35;Capture("launcher-shader-prompt");surface.Activate("shader_prompt:later");Check(prompt.Result=="later","unfinished shaders can be postponed");
    Check(surface.UpdatePromptEnabled,"update notifications default to enabled with no preference file");
    surface.SetUpdateResult("available","Synthetic update available.");prompt=surface.ShowUpdatePrompt();surface.FrozenTime=36;Capture("launcher-update-prompt");surface.Activate("update_prompt:dont");surface.Activate("update_prompt:later");Check(prompt.Result=="later"&&!surface.UpdatePromptEnabled,"original update opt-out persists in project configuration");
    var importChoice=surface.ChooseModImport();surface.Activate("modal:mid");Check(importChoice.Result=="link","Add mod preserves symbolic-link option");
    surface.Navigate("home");surface.PluginBuildActivity(true,"Compiling plugin 42%, about 3 min left");surface.FrozenTime=37;Capture("launcher-plugin-progress");surface.PluginBuildActivity(false);
    surface.ShaderActivity(true,"42 of 100 shaders, about a minute left");surface.FrozenTime=38;Capture("launcher-shader-progress");surface.ShaderActivity(false);

    surface.FrozenTime=20;surface.Navigate("mods_browser");surface.FrozenTime=25;Capture("launcher-marketplace-heading");surface.Activate("nav:back");
    Check(surface.CurrentView=="mods","marketplace Back returns to the installed mods screen");
    surface.FrozenTime=20;surface.Navigate("setup_modding");surface.FrozenTime=25;Capture("launcher-plugins");
    surface.Navigate("home");surface.FrozenTime=30;surface.InvalidateVisual();using(var frame=window.CaptureRenderedFrame()){}
    window.MouseDown(new Avalonia.Point(1080,36),Avalonia.Input.MouseButton.Left);
    window.MouseUp(new Avalonia.Point(1080,36),Avalonia.Input.MouseButton.Left);
    Check(surface.CurrentView=="settings","original header hit targets navigate through real pointer events");
    surface.SetProject(project,forceInstaller:true);
    Check(surface.IsInstaller,"explicit installer mode still opens setup for built projects");
    window.Close();
    var firstRun=new MainWindow(["--first-run"],root,enableBackgroundActions:false);
    firstRun.Show();firstRun.Width=1440;firstRun.Height=810;var onboarding=firstRun.Surface;onboarding.FrozenTime=5;
    Check(onboarding.IsFirstRun,"a standalone first run opens location selection before the installer");
    string sampleParent=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),"Games");
    onboarding.SetBootstrapParent(sampleParent);
    Check(onboarding.BootstrapTarget==Path.Combine(sampleParent,"OpenSmash"),"first-run location always shows the OpenSmash child folder");
    using(var frame=firstRun.CaptureRenderedFrame()){Check(frame!=null,"renders the styled first-run location screen");if(screenshot!=null)frame!.Save(Path.Combine(Path.GetDirectoryName(screenshot)!,"bootstrap-choose.png"));}
    onboarding.StartBootstrap();onboarding.BootstrapProgress(new BootstrapProgress("download",25,100));
    using(var frame=firstRun.CaptureRenderedFrame()){Check(frame!=null,"renders first-run download progress");if(screenshot!=null)frame!.Save(Path.Combine(Path.GetDirectoryName(screenshot)!,"bootstrap-download.png"));}
    onboarding.FinishBootstrap("Synthetic connection failure. Retry the download.");
    using(var frame=firstRun.CaptureRenderedFrame()){Check(frame!=null,"renders retryable first-run errors");if(screenshot!=null)frame!.Save(Path.Combine(Path.GetDirectoryName(screenshot)!,"bootstrap-error.png"));}
    firstRun.Close();
    var moved=new MainWindow([],empty,enableBackgroundActions:false);
    Check(moved.Surface.IsFirstRun,"moving the launcher to an empty folder shows download setup, regardless of working directory or remembered projects");moved.Close();
    var local=new MainWindow([],root,enableBackgroundActions:false);
    Check(local.Surface.CurrentView=="home","placing the launcher beside a built game shows the regular launcher");local.Close();
    string fresh=Path.Combine(root,"new-project");Directory.CreateDirectory(Path.Combine(fresh,"runtime"));Directory.CreateDirectory(Path.Combine(fresh,"tools"));File.WriteAllText(Path.Combine(fresh,"tools","os_mods.py"),"");
    var unbuilt=new MainWindow([],fresh,enableBackgroundActions:false);
    Check(unbuilt.Surface.CurrentView=="installer:setup","placing the launcher beside an unbuilt project shows the installer");unbuilt.Close();
    ChangeChecks.Controller(root);
    UpstreamChecks.Run(root);
    Console.WriteLine("All desktop launcher checks passed.");
}
finally
{
    // Release any owned startup fixture before removing its unique test folder.
    if(Directory.Exists(root)){File.WriteAllText(Path.Combine(root,"game-survived.txt.release"),"");await Task.Delay(150).ConfigureAwait(false);}
    // root is a freshly-created, unique temp directory owned only by this test.
    Directory.Delete(root, recursive: true);
}
