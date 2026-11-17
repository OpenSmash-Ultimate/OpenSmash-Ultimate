"""A standard plugin built for an older game version is ported to 13.0.5 with an address map made for
one build of it (tools/ports/<module>-<version>.txt; its "sha256" line names that build).
tools/std_plugins.py pins the build it downloads (PLUGINS: version, SHA-256) and names the map
(PORTS). fetch() checks the download against the pin and then ports it with the map, whatever build
the map was made for, so a pin moved to a new release with the map left behind would rewrite the
new build with the old build's addresses. Here: every map is the one made for the pinned build.
No game files and no download needed."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import port_plugin  # noqa: E402
import std_plugins  # noqa: E402


class PortMaps(unittest.TestCase):
    def test_each_map_is_made_for_the_pinned_build(self):
        for name, map_name in std_plugins.PORTS.items():
            with self.subTest(plugin=name):
                version, _url, _member, sha = std_plugins.PLUGINS[name][:4]
                _table, made_for = port_plugin.load_map(ROOT / 'tools' / 'ports' / map_name)
                self.assertEqual(made_for, sha)

    def test_each_map_is_named_for_the_pinned_version(self):
        for name, map_name in std_plugins.PORTS.items():
            with self.subTest(plugin=name):
                module = name.removeprefix('lib').removesuffix('.nro')
                self.assertEqual(map_name, f'{module}-{std_plugins.PLUGINS[name][0]}.txt')


if __name__ == '__main__':
    unittest.main()
