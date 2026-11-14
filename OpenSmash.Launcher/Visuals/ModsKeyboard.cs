using Avalonia.Input;
namespace OpenSmash.Visuals;
public sealed partial class ClassicSurface
{
    bool HandleModsKey(Key key)
    {
        if(_openDropdownKey?.StartsWith("modpick:")==true)
        {
            var line=_modLines.FirstOrDefault(m=>"modpick:"+m.GroupKey==_openDropdownKey);
            if(line==null){_openDropdownKey=null;return true;}
            if(key is Key.Up or Key.Down)_dropdownHoverIndex=Math.Clamp(_dropdownHoverIndex+(key==Key.Up?-1:1),0,line.Names.Length-1);
            if(key==Key.Enter&&_dropdownHoverIndex>=0){ActionRequested?.Invoke("modvariant:"+line.Names[_dropdownHoverIndex]);_openDropdownKey=null;}
            if(key==Key.Escape)_openDropdownKey=null;
            return true;
        }
        if(key is Key.Up or Key.Down)
        {
            _keys=true;_sel=Math.Clamp(_sel+(key==Key.Up?-1:1),0,Math.Max(0,ModActionCount+_modLines.Count-1));
            int row=_sel-ModActionCount;
            if(row>=0){int top=_modLines.Take(row).Sum(m=>m.RowH);int bottom=top+_modLines[row].RowH;int view=(int)ModListView;if(top<_modScroll)_modScroll=top;else if(bottom>_modScroll+view)_modScroll=Math.Max(0,bottom-view);}
            return true;
        }
        if(key==Key.Delete&&_sel>=ModActionCount&&!Busy){ActionRequested?.Invoke("act:remove");return true;}
        if(key==Key.Enter&&_sel<ModActionCount){Activate(new[]{"act:browse","act:setup","act:check","act:share"}[_sel]);return true;}
        return false;
    }
}
