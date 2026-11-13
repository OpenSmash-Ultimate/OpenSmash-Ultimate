namespace OpenSmash.Visuals;

public sealed partial class ClassicSurface
{
    bool _sharePrompt, _checkModsPrompt;
    double _sharePromptAt, _checkModsPromptAt;
    string _checkModsText = "";
    List<(string gb, string file, string title)> _checkModsUpdates = [];
    public IReadOnlyList<(string gb, string file, string title)> ModUpdates => _checkModsUpdates;
    public void ShowSharePrompt() { _sharePromptAt = Now; _sharePrompt = true; InvalidateVisual(); }
    public void ShowModCheck(string text, List<(string gb, string file, string title)> updates)
    { _checkModsText = text; _checkModsUpdates = updates; _checkModsPromptAt = Now; _checkModsPrompt = true; InvalidateVisual(); }

    bool ActivateModDialog(string id)
    {
        if (!_sharePrompt && !_checkModsPrompt) return false;
        switch (id)
        {
            case "share_prompt:cancel": _sharePrompt = false; break;
            case "share_prompt:share": _sharePrompt = false; ActionRequested?.Invoke("mods:export"); break;
            case "share_prompt:install": _sharePrompt = false; ActionRequested?.Invoke("mods:import"); break;
            case "check_prompt:close": _checkModsPrompt = false; break;
            case "check_prompt:install": _checkModsPrompt = false; ActionRequested?.Invoke("mods:update"); break;
        }
        InvalidateVisual(); return true;
    }
}
