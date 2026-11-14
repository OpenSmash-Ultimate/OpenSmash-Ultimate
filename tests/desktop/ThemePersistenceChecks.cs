using System.Reflection;
using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Threading;
using OpenSmash;
using OpenSmash.Core;
using OpenSmash.Visuals;

static class ThemePersistenceChecks
{
    static void Check(bool ok, string message) { if (!ok) throw new Exception(message); Console.WriteLine("PASS " + message); }
    static FieldInfo Field(string name) => typeof(ClassicSurface).GetField(name,BindingFlags.Instance|BindingFlags.NonPublic)!;
    static void CreateProject(string root)
    {
        Directory.CreateDirectory(Path.Combine(root,"runtime"));
        Directory.CreateDirectory(Path.Combine(root,"tools"));
        File.WriteAllText(Path.Combine(root,"tools","os_mods.py"),"");
    }

    public static void Run(string temporary)
    {
        string root=Path.Combine(temporary,"theme-save-dialog"); CreateProject(root);
        Directory.CreateDirectory(Path.Combine(root,"config"));
        File.WriteAllText(Path.Combine(root,"config","version.ini"),"version = 1");
        string config=Path.Combine(root,"config","launcher.ini");
        File.WriteAllText(config,"[other]\nkeep = yes\n");
        var window=new MainWindow([],root,enableBackgroundActions:false); window.Show();
        var surface=window.Surface;
        surface.ChangeSetting("theme_preset","builtin:Nord");
        var settings=(Array)Field("_settings").GetValue(surface)!;
        int saveIndex=Array.FindIndex(settings.Cast<object>().ToArray(),s=>(string)s.GetType().GetField("Key")!.GetValue(s)! == "theme_save");
        surface.Activate("set:"+saveIndex);
        var dialog=window.OwnedWindows.Single(w=>w.Title=="Save theme preset");
        var panel=(StackPanel)dialog.Content!;
        string name="My; # Nord";
        ((TextBox)panel.Children[1]).Text=name;
        var save=(Button)((StackPanel)panel.Children[2]).Children[1];
        Check(save.IsEnabled,"named preset dialog enables Save for a valid name");
        save.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
        if(!SpinWait.SpinUntil(()=>{Dispatcher.UIThread.RunJobs();return !(bool)Field("_presetDialogOpen").GetValue(surface)!;},3000))
            throw new TimeoutException("Preset save did not finish.");
        Check(surface.GetSetting("theme_preset")=="saved:"+name && surface.GetSetting("theme_accent")=="#88C0D0", "saving a named preset selects it without changing or resetting the colors");
        string presets=Path.Combine(root,"config","launcher-themes.json");
        byte[] saved=File.ReadAllBytes(presets);
        Check(File.ReadAllText(config).Contains("keep = yes")&&File.ReadAllText(config).Contains("preset = saved%3A"),"saving a preset atomically persists its selection and preserves unrelated settings");
        window.Close();
        var reopened=new MainWindow([],root,enableBackgroundActions:false);
        Check(reopened.Surface.GetSetting("theme_preset")=="saved:"+name,"saved preset name restores even when its colors equal a built-in palette");
        Check(reopened.Surface.GetSetting("theme_accent")=="#88C0D0"&&reopened.Surface.GetSetting("theme_background")=="#2E3440"&&reopened.Surface.GetSetting("theme_text")=="#ECEFF4","all saved colors restore on reopening a migrated project");
        Check(saved.SequenceEqual(File.ReadAllBytes(presets)),"reopening does not rewrite or lose named presets");
        reopened.Surface.ChangeSetting("theme_accent","#123456"); reopened.Close();
        var custom=new MainWindow([],root,enableBackgroundActions:false);
        Check(custom.Surface.GetSetting("theme_preset")=="custom"&&custom.Surface.GetSetting("theme_accent")=="#123456","manual edits persist as custom instead of reapplying the previous preset");
        custom.Close();

        string migrated=Path.Combine(temporary,"theme-config-migration"); CreateProject(migrated);
        string original=Path.Combine(migrated,"launcher-themes.json");
        ThemePresets.Save(original,new ThemePreset("Before migration","#ABCDEF","#112233","#F0F0F0"));
        byte[] before=File.ReadAllBytes(original);
        Directory.CreateDirectory(Path.Combine(migrated,"config"));
        File.WriteAllText(Path.Combine(migrated,"config","version.ini"),"version = 1");
        File.WriteAllText(Path.Combine(migrated,"config","launcher.ini"),"[theme]\npreset = saved%3ABefore%20migration\n");
        var afterMigration=new MainWindow([],migrated,enableBackgroundActions:false);
        Check(afterMigration.Surface.GetSetting("theme_preset")=="saved:Before migration"&&afterMigration.Surface.GetSetting("theme_accent")=="#ABCDEF","config-folder migration still finds presets saved beside the launcher");
        Check(before.SequenceEqual(File.ReadAllBytes(original)),"loading legacy presets preserves their original file");
        afterMigration.Close();
    }
}
