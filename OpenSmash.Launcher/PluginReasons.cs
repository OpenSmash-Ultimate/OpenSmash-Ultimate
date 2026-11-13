namespace OpenSmash;
public sealed partial class MainWindow
{
        static string PluginRefreshWhy(List<string> waiting, string reason)
        {
            if (waiting.Count == 1)
                return waiting[0] + " has a new plugin file. Refreshing rebuilds its DLL. The game program stays as it is.";
            if (waiting.Count > 1)
                return "These mods have new plugin files. Refreshing rebuilds their DLLs. The game program stays as it is.\n\n" +
                    string.Join("\n", waiting.Take(8));
            if (!string.IsNullOrEmpty(reason))
                return "A moveset plugin has to be turned into a DLL again. The game program stays as it is.";
            return "";
        }

        static string PluginUpdateWhy(List<string> waiting, string reason)
        {
            var parts = new List<string>();
            if (waiting.Count == 1)
                parts.Add(waiting[0] + " has a plugin that isn't ready yet. A match loads the vanilla fighter until it is updated.");
            else if (waiting.Count > 1)
                parts.Add("These mods have plugins that aren't ready yet. A match loads the vanilla fighter until they are updated.\n\n" +
                    string.Join("\n", waiting.Take(8)));
            bool sameAsWaiting = reason.Contains("isn't translated") || reason.Contains("isn't recompiled");
            if (!string.IsNullOrEmpty(reason) && !(waiting.Count > 0 && sameAsWaiting))
            {
                if (reason.Contains("hooks a function"))
                    parts.Add("A mod hooks the middle of a function. The game has to be updated once so that hook can run.");
                else if (reason.StartsWith("the plugin "))
                    parts.Add(reason.Substring("the plugin ".Length).Replace(" isn't in the game", "") + " was added. The game has to be updated once so that plugin can run.");
                else if (reason.Contains("isn't in the game") || reason.Contains("isn't fetched"))
                    parts.Add("A community plugin the mods rely on isn't in the game yet.");
                else if (reason.Contains("patches.txt"))
                    parts.Add("A mod patches the game's code. The game has to be updated once so that patch is in it.");
                else if (reason.Contains("newer than"))
                    parts.Add("The game itself changed. It has to be linked once so Play uses the current code.");
                else if (sameAsWaiting)
                    parts.Add("A moveset's plugin still has to be turned into a DLL.");
                else if (reason.Contains("isn't built"))
                    parts.Add("The game isn't built yet.");
                else
                    parts.Add(reason);
            }
            return string.Join("\n\n", parts);
        }

}
