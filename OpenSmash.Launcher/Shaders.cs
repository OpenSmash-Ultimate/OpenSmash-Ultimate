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
        string source=File.ReadAllText(header),text=File.ReadAllText(marker);
        var version=Regex.Match(source,@"#define SHADER_CACHE_VERSION (\d+)");
        if(!version.Success||!Regex.IsMatch(text,@"\bversion "+version.Groups[1].Value+@"\b"))return false;
        // An older SPIR-V version's files have other names (#330), so only this version's "spirv g<version>." counts.
        var spv=Regex.Match(source,@"#define SHADER_CACHE_SPV_VERSION \(SHADER_CACHE_VERSION \* 100 \+ (\d+)\)");
        string want=spv.Success?"\nspirv g"+(int.Parse(version.Groups[1].Value)*100+int.Parse(spv.Groups[1].Value))+".":"\nspirv ";
        string renderer=Surface.GetSetting("renderer");
        return renderer=="direct3d11"||!File.Exists(p.PathIn("build","ssbu_glslang.dll"))||text.Contains(want);
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
