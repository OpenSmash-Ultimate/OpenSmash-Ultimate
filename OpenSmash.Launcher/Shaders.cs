using System.Text.RegularExpressions;
using Avalonia.Threading;
using OpenSmash.Core;
namespace OpenSmash;
public sealed partial class MainWindow
{
    CancellationTokenSource? _shaderCancel;
    bool ShadersDone()
    {
        var p=RequireProject();string marker=p.PathIn("dump","shadercache","complete.txt"),header=p.PathIn("runtime","gpu_shader_cache.h");
        if(!File.Exists(marker)||!File.Exists(header))return false;
        var version=Regex.Match(File.ReadAllText(header),@"#define SHADER_CACHE_VERSION (\d+)");
        if(!version.Success||!Regex.IsMatch(File.ReadAllText(marker),@"\bversion "+version.Groups[1].Value+@"\b"))return false;
        string renderer=Surface.GetSetting("renderer");
        return renderer=="direct3d11"||!File.Exists(p.PathIn("build","ssbu_glslang.dll"))||File.ReadAllText(marker).Contains("\nspirv ");
    }
    async Task AskShaders()
    {
        if(_closed||_project?.IsBuilt!=true||ShadersDone()||Surface.HasPrompt||_shaderCancel!=null)return;
        string priority=await Surface.ShowShaderPrompt();
        if(priority is "low" or "medium" or "high")_=CompileShaders(priority);
    }
    async Task CompileShaders(string priority)
    {
        if(_shaderCancel!=null||_closed)return;
        if(_project?.IsBuilt!=true){Surface.Alert("BUILD THE GAME FIRST","Finish installing the game before compiling shaders.");return;}
        var p=RequireProject();using var cancel=new CancellationTokenSource();_shaderCancel=cancel;Surface.ShaderActivity(true);
        try
        {
            int exit=await ProcessRunner.Run(new Command(p.Python,[p.PathIn("tools","shader_aot.py"),"--priority",priority],p.Root),
                line=>Dispatcher.UIThread.Post(()=>
                {
                    if(!_closed&&ReferenceEquals(_shaderCancel,cancel))Surface.ShaderActivity(true,line);
                }),cancel.Token,logPath:p.PathIn("build","logs","shader_window.log"));
            if(exit!=0&&!_closed)Surface.Alert("SHADER COMPILE STOPPED","See build/logs/shader_window.log. The next start resumes completed work.");
        }
        catch(OperationCanceledException){}
        catch(Exception error){if(!_closed)Surface.Alert("SHADER COMPILE STOPPED",error.Message);}
        finally{_shaderCancel=null;Surface.ShaderActivity(false);}
    }
}
