// (v0.5.5) What the setup error reports need of the surface (MainWindow.SetupReports.cs, OpenSmash.Core SetupReport.cs):
// the failed step and error of the last install, and the report's line (its error code) shown where the player reads the error.
namespace OpenSmash.Visuals;
public sealed partial class ClassicSurface
{
    /// <summary>the last install's error and its failed step ("id title"), "" when not known</summary>
    public (string Error,string Step) InstallFailureInfo()
    {
        var step=_steps.FirstOrDefault(s=>s.State=="fail");
        return (_errMsg,step==null?"":(step.Id+" "+step.Title).Trim());
    }

    /// <summary>the installer log's last lines</summary>
    public string InstallLogTail(int lines)=>string.Join("\n",_log.Skip(Math.Max(0,_log.Count-lines)));

    /// <summary>the install's error panel: the line under the error</summary>
    public void ShowInstallReportLine(string line)
    {
        if(_running)return;
        _errHint=line;InvalidateVisual();
    }

    /// <summary>the report's line, where the error is shown: an open message, the download's error, the install's panel;
    /// before (the line shown at once) is replaced when it is there, else line is added</summary>
    public void ShowReportLine(string? before,string line)
    {
        static string Put(string text,string? old,string now)=>!string.IsNullOrEmpty(old)&&text.Contains(old)?text.Replace(old,now):text.Length==0?now:text+"\n\n"+now;
        if(_modal!=null&&(!string.IsNullOrEmpty(before)&&_modal.Body.Contains(before)))_modal.Body=Put(_modal.Body,before,line);
        if(!string.IsNullOrEmpty(before)&&_bootstrapError.Contains(before))_bootstrapError=Put(_bootstrapError,before,line);
        if(!string.IsNullOrEmpty(before)&&_errHint==before)_errHint=line;
        InvalidateVisual();
    }
}
