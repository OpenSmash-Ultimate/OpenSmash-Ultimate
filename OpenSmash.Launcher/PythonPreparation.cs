using OpenSmash.Core;
namespace OpenSmash;
public sealed partial class MainWindow
{
    Task? _pythonPreparation;
    Task EnsureToolsPython(bool force=false)
    {
        if(_pythonPreparation is {IsCompleted:false})return _pythonPreparation;
        return _pythonPreparation=PrepareToolsPython(force);
    }
    async Task PrepareToolsPython(bool force)
    {
        var p=RequireProject();if(p.Platform!=DesktopPlatform.Windows)return;
        string script=p.PathIn("tools","install_windowsmash.ps1");
        if(!File.Exists(script)||!File.ReadAllText(script).Contains("[switch]$PythonOnly"))return;
        string own=p.PathIn("deps","venv","Scripts","python.exe");
        if(!force&&File.Exists(p.PathIn("deps","venv",".opensmash-ready"))&&File.Exists(own))return;
        string bundled=p.PathIn("deps","python","python.exe");
        if(!force&&File.Exists(bundled))
        {
            int ready=await ProcessRunner.Run(new Command(bundled,["-c","import Cryptodome"],p.Root),_=>{},CancellationToken.None);
            if(ready==0)return;
        }
        bool ownsBusy=!Surface.OperationRunning;
        if(ownsBusy){Surface.SetOperationLabel("SETTING UP PYTHON");Surface.OperationRunning=true;}
        try
        {
            int code=await ProcessRunner.Run(new Command("powershell.exe",["-NoProfile","-ExecutionPolicy","Bypass","-File",script,"-PythonOnly"],p.Root),_=>{},CancellationToken.None,logPath:p.PathIn("build","logs","python_setup.log"));
            if(code!=0)throw new InvalidOperationException("Python setup stopped. See build/logs/python_setup.log; try again when the connection is available.");
        }
        finally{if(ownsBusy){Surface.OperationRunning=false;Surface.InvalidateVisual();}}
    }
}
