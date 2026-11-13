using System.Text.RegularExpressions;
namespace OpenSmash.Visuals;
public sealed partial class ClassicSurface
{
    bool _shaderUi,_pluginBuildUi;
    float _shaderFrac,_pluginFrac=-1;
    string _shaderDetail="",_shaderEta="",_pluginDetail="",_pluginEta="";
    public void PluginBuildActivity(bool active,string line="")
    {
        if(active&&!_pluginBuildUi){_pluginFrac=-1;_pluginDetail="Starting the plugin build";_pluginEta="";}
        _pluginBuildUi=active;if(line.Length>0)NotePluginBuild(line);InvalidateVisual();
    }
    public void ShaderActivity(bool active,string line="")
    {
        if(active&&!_shaderUi){_shaderFrac=0;_shaderDetail="Listing shader files";_shaderEta="";}
        _shaderUi=active;if(line.Length>0)NoteShaderLine(line);InvalidateVisual();
    }
        void NotePluginBuild(string text)
        {
            var step = Regex.Match(text, @"^(.*?)(\d+)\s*%\s*,?\s*(.*)$");
            if (step.Success && int.TryParse(step.Groups[2].Value, out int pct))
            {
                string detail = Regex.Replace(step.Groups[1].Value, @"[^A-Za-z0-9.)\]]+$", "").Trim();
                _pluginDetail = detail.Length > 0 ? detail : text;
                _pluginFrac = Math.Clamp(pct / 100f, 0f, 1f);
                _pluginEta = step.Groups[3].Value.Trim().TrimStart(',', ' ');
            }
            else if (!text.Contains("recomp.py") && text.Length < 240)
                _pluginDetail = text;
            InvalidateVisual();
        }
        void NoteShaderLine(string line)
        {
            var files = Regex.Match(line, @"(listing|reading) shader files:\s*([\d,]+)\s+of\s+([\d,]+)", RegexOptions.IgnoreCase);
            if (files.Success
                && double.TryParse(files.Groups[2].Value.Replace(",", ""), out double read)
                && double.TryParse(files.Groups[3].Value.Replace(",", ""), out double filesTotal)
                && filesTotal > 0)
            {
                _shaderFrac = (float)(read / filesTotal);
                string phase = files.Groups[1].Value.Equals("listing", StringComparison.OrdinalIgnoreCase)
                    ? "Listing shader files" : "Reading shader files";
                _shaderDetail = $"{phase}  {read:N0} / {filesTotal:N0}";
                _shaderEta = "";
                InvalidateVisual();
                return;
            }
            var count = Regex.Match(line, @"([\d,]+)\s+of\s+([\d,]+)\s+shaders(?:,\s*(.+))?$", RegexOptions.IgnoreCase);
            if (count.Success
                && double.TryParse(count.Groups[1].Value.Replace(",", ""), out double done)
                && double.TryParse(count.Groups[2].Value.Replace(",", ""), out double total)
                && total > 0)
            {
                _shaderFrac = (float)(done / total);
                _shaderDetail = $"Compiling shaders  {done:N0} / {total:N0}";
                _shaderEta = count.Groups[3].Success ? count.Groups[3].Value.Trim() : "";
            }
            else if (line.StartsWith("getting ready", StringComparison.OrdinalIgnoreCase))
            {
                _shaderFrac = 0;
                _shaderDetail = "Translating vertex programs";
                _shaderEta = "";
            }
            else if (line.StartsWith("compiling with", StringComparison.OrdinalIgnoreCase))
            {
                _shaderFrac = 0;
                _shaderDetail = "Compiling shaders";
                _shaderEta = "";
            }
            InvalidateVisual();
        }
}
