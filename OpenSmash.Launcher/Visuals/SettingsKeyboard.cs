using Avalonia.Input;
namespace OpenSmash.Visuals;
public sealed partial class ClassicSurface
{
    bool HandleSettingsKey(Key key)
    {
        var visible=GetVisibleSettings();
        var selected=_sel>=0&&_sel<visible.Length?visible[_sel]:null;
        if(_openDropdownKey!=null)
        {
            var setting=_settings.FirstOrDefault(s=>s.Key==_openDropdownKey);
            if(key==Key.Escape){_openDropdownKey=null;return true;}
            if(setting!=null&&key is Key.Up or Key.Down)
            {_dropdownHoverIndex=Math.Clamp(_dropdownHoverIndex+(key==Key.Up?-1:1),0,setting.Values.Length-1);return true;}
            if(setting!=null&&key==Key.Enter&&_dropdownHoverIndex>=0)
            {ChangeSetting(setting.Key,setting.Values[_dropdownHoverIndex]);_openDropdownKey=null;return true;}
        }
        if(key is Key.Up or Key.Down)
        {
            _keys=true;int start=SectionStart(_settingsTab);_sel=Math.Clamp(_sel+(key==Key.Up?-1:1),start,Math.Max(start,start+_settingsTabs[_settingsTab].SettingKeys.Length-1));
            if(_sel<_rowTop.Length){float top=_rowTop[_sel],bottom=top+_settingsRowH;if(top<_settingsScroll)_settingsScroll=top;else if(bottom>_settingsScroll+_settingsViewH)_settingsScroll=Math.Clamp(bottom-_settingsViewH,0,_settingsMaxScroll);}
            return true;
        }
        if(key is Key.Left or Key.Right)
        {
            _keys=true;
            if(selected?.Slider==true)
            {int.TryParse(GetSetting(selected.Key),out int value);ChangeSetting(selected.Key,Math.Clamp(value+(key==Key.Left?-5:5),0,100).ToString());}
            else{CommitEdit();_settingsTab=(_settingsTab+(key==Key.Left?_settingsTabs.Length-1:1))%_settingsTabs.Length;_settingsScroll=0;_openDropdownKey=null;}
            return true;
        }
        if(key==Key.Enter)
        {
            if(_settingsTabs[_settingsTab].Name.Equals("Update",StringComparison.OrdinalIgnoreCase))ActionRequested?.Invoke(_updateState=="available"?"update:apply":"update:check");
            else if(selected!=null&&!selected.Slider)Activate("set:"+Array.IndexOf(_settings,selected));
            return true;
        }
        return false;
    }
}
