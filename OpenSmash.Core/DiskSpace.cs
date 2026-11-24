// "Save disk space" (the launcher's Settings > Storage): one setting, config\save_disk.txt ("on" / "off", off when the
// file is missing), kept with the other settings so the installer and every update read the same copy.
//
// On, the build is run as with SSBU_DATA_IN_PLACE=1 and SSBU_TRIM=c,nsp,map (tools/build_game.py save_disk_apply,
// tools/install_opensmash.ps1): the game's data is read from the game's own file instead of a decrypted copy
// (about 14 GB less: the game files must stay where they are), and the disk a finished build no longer needs is given
// back (the generated C, the plain copy of a compressed package, the map file). A variable set by hand wins over it.
// The data part counts when the game's data is set up (a new install, or the set-up run again); the trim after the
// next build.
using System;
using System.IO;

namespace OpenSmash.Core
{
    public static class DiskSpace
    {
        public const string SettingFile = "save_disk.txt";
        /// <summary>what the build is run with when the setting is on (the installer's own names)</summary>
        public const string DataInPlace = "SSBU_DATA_IN_PLACE", Trim = "SSBU_TRIM", TrimWhat = "c,nsp,map";

        public static bool Saving(string root)
        {
            try
            {
                string f = Path.Combine(root, "config", SettingFile);
                return File.Exists(f) && File.ReadAllText(f).Trim().StartsWith("on", StringComparison.OrdinalIgnoreCase);
            }
            catch { return false; }
        }

        public static void SetSaving(string root, bool on)
        {
            Directory.CreateDirectory(Path.Combine(root, "config"));
            File.WriteAllText(Path.Combine(root, "config", SettingFile), on ? "on\n" : "off\n");
        }
    }
}
