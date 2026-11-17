using System.Text.Json;

namespace OpenSmash.Core;

// Project-local acknowledgement: status scans may add folders to mods.json, so that
// index cannot acknowledge an external addition after the first scan.
public sealed class ModChanges(Project project)
{
    string KnownFile => project.PathIn("build", "work", "known_mods.txt");
    public string[] Scan()
    {
        string folder=project.PathIn("mods");
        if(!Directory.Exists(folder))return [];
        var present=Directory.GetDirectories(folder).Select(Path.GetFileName).OfType<string>()
            .Where(n=>!n.StartsWith('.')).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var known=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        if(File.Exists(KnownFile))known.UnionWith(File.ReadAllLines(KnownFile).Select(n=>n.Trim()).Where(n=>n.Length>0));
        else
        {
            string index=Path.Combine(folder,"mods.json");
            if(File.Exists(index))
            {
                using var doc=JsonDocument.Parse(File.ReadAllText(index));
                if(doc.RootElement.TryGetProperty("mods",out var mods)&&mods.ValueKind==JsonValueKind.Object)
                    known.UnionWith(mods.EnumerateObject().Select(p=>p.Name));
            }
            else known.UnionWith(present); // Existing projects without an index establish a baseline.
        }
        known.IntersectWith(present);
        Save(known);
        return present.Except(known,StringComparer.OrdinalIgnoreCase).Order(StringComparer.OrdinalIgnoreCase).ToArray();
    }
    public void Remember(IEnumerable<string> names)
    {
        var known=File.Exists(KnownFile)?File.ReadAllLines(KnownFile).ToHashSet(StringComparer.OrdinalIgnoreCase):new(StringComparer.OrdinalIgnoreCase);
        known.UnionWith(names);Save(known);
    }
    void Save(IEnumerable<string> names)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(KnownFile)!);
        string temporary=KnownFile+".tmp";
        File.WriteAllLines(temporary,names.Order(StringComparer.OrdinalIgnoreCase));File.Move(temporary,KnownFile,true);
    }
    public bool HasPlugin(string name)
    {
        string folder=project.PathIn("mods",name);
        return Directory.Exists(folder)&&Directory.EnumerateFiles(folder,"*",SearchOption.AllDirectories)
            .Any(f=>Path.GetExtension(f).Equals(".nro",StringComparison.OrdinalIgnoreCase)&&
                !Path.GetRelativePath(folder,f).Replace('\\','/').StartsWith("prebuilt/",StringComparison.OrdinalIgnoreCase));
    }
}
