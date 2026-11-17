// Original Windows launcher definitions, now maintained in the shared implementation.
#nullable disable
namespace OpenSmash.Visuals;
public sealed partial class ClassicSurface
{
        void BuildModLines()
        {
            var lines = new List<ModLine>();
            var seen = new HashSet<string>();
            foreach (var m in _mods)
            {
                if (string.IsNullOrEmpty(m.Group))
                {
                    lines.Add(DetailLine(m, string.IsNullOrEmpty(m.Title) ? m.Name : m.Title, m.Name));
                    continue;
                }
                if (!seen.Add(m.Group)) continue;
                var parts = _mods.Where(x => x.Group == m.Group).ToArray();
                int on = parts.Count(p => p.Enabled);
                string worst = parts.Select(p => p.Online).FirstOrDefault(o => o == "unsafe")
                    ?? parts.Select(p => p.Online).FirstOrDefault(o => o == "caution")
                    ?? parts.Select(p => p.Online).FirstOrDefault(o => o == "same")
                    ?? parts.Select(p => p.Online).FirstOrDefault(o => o == "safe")
                    ?? "";
                // UI color packs: one row + dropdown to pick the active variant (only one on).
                if (parts.Any(p => p.Pick))
                {
                    int active = Array.FindIndex(parts, p => p.Enabled);
                    if (active < 0) active = 0;
                    var activePart = parts[active];
                    var meta = new List<string>();
                    if (!string.IsNullOrEmpty(activePart.Category)) meta.Add(activePart.Category.ToUpperInvariant());
                    if (!string.IsNullOrEmpty(activePart.Author)) meta.Add("BY " + activePart.Author.ToUpperInvariant());
                    lines.Add(new ModLine
                    {
                        Pick = true,
                        GroupKey = m.Group,
                        Title = string.IsNullOrEmpty(m.GroupTitle) ? m.Group : m.GroupTitle,
                        Sub = parts.Length + " COLORS",
                        Files = activePart.Files,
                        Enabled = on > 0,
                        Thumb = "g:" + m.Group,
                        Names = parts.Select(p => p.Name).ToArray(),
                        Variants = parts.Select(p => string.IsNullOrEmpty(p.Variant)
                            ? (string.IsNullOrEmpty(p.Part) ? p.Name : p.Part)
                            : p.Variant).ToArray(),
                        Active = active,
                        Meta = string.Join("   ·   ", meta),
                        About = activePart.About ?? "",
                        Online = worst,
                        NewSlots = parts.Any(p => p.NewSlots),
                        Code = parts.Any(p => p.Code == "waiting") ? "waiting" : parts.Any(p => p.Code == "built") ? "built" : "",
                        Added = parts.Sum(p => p.Added),
                        Merged = parts.Sum(p => p.Merged),
                        Skipped = parts.Sum(p => p.Skipped),
                        KeptShared = parts.Sum(p => p.KeptShared),
                    });
                    continue;
                }
                if (_knownGroups.Add(m.Group)) _openGroups.Add(m.Group);
                bool open = _openGroups.Contains(m.Group);
                lines.Add(new ModLine
                {
                    Header = true,
                    Open = open,
                    Title = string.IsNullOrEmpty(m.GroupTitle) ? m.Group : m.GroupTitle,
                    Sub = parts.Length + " PARTS" + (on == parts.Length ? "" : "  ·  " + on + " ON"),
                    Files = parts.Sum(p => p.Files),
                    Enabled = on == parts.Length,
                    Mixed = on > 0 && on < parts.Length,
                    Thumb = "g:" + m.Group,
                    Names = parts.Select(p => p.Name).ToArray(),
                    Online = worst,
                    NewSlots = parts.Any(p => p.NewSlots),
                    Code = parts.Any(p => p.Code == "waiting") ? "waiting" : parts.Any(p => p.Code == "built") ? "built" : "",
                });
                if (!open) continue;
                foreach (var p in parts)
                    lines.Add(DetailLine(p, string.IsNullOrEmpty(p.Part) ? p.Name : p.Part, null, child: true));
            }
            foreach (var line in lines) line.RowH = ModLineHeight(line);
            _modLines = lines;
            if (_openDropdownKey != null && _openDropdownKey.StartsWith("modpick:")
                && !_modLines.Any(l => l.Pick && "modpick:" + l.GroupKey == _openDropdownKey))
                _openDropdownKey = null;
        }

        ModLine DetailLine(ModInfo m, string title, string thumb, bool child = false)
        {
            var meta = new List<string>();
            if (!string.IsNullOrEmpty(m.Category)) meta.Add(m.Category.ToUpperInvariant());
            if (!string.IsNullOrEmpty(m.Author)) meta.Add("BY " + m.Author.ToUpperInvariant());
            return new ModLine
            {
                Child = child,
                Title = title,
                Files = m.Files,
                Enabled = m.Enabled,
                Thumb = thumb ?? "",
                Names = new[] { m.Name },
                Meta = string.Join("   ·   ", meta),
                About = m.About ?? "",
                Online = m.Online ?? "",
                NewSlots = m.NewSlots,
                Code = m.Code ?? "",
                Added = m.Added,
                Merged = m.Merged,
                Skipped = m.Skipped,
                KeptShared = m.KeptShared,
            };
        }

        static int ModLineHeight(ModLine m)
        {
            int h = 22 + 22;
            if (!string.IsNullOrEmpty(m.Sub)) h += 18;
            if (m.HasChips || !string.IsNullOrEmpty(m.Meta)) h += 26;
            if (!string.IsNullOrEmpty(m.About)) h += 44;
            return Math.Max(68, h + 16);
        }
}
