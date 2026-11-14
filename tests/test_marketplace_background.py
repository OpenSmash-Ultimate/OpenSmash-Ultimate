from pathlib import Path
import unittest


class MarketplaceBackgroundTests(unittest.TestCase):
    def test_page_and_native_view_leave_launcher_background_visible(self):
        root = Path(__file__).resolve().parent.parent
        page = (root / "OpenSmash.Launcher/Assets/MarketplaceHtml.html").read_text(encoding="utf-8")
        host = (root / "OpenSmash.Launcher/Visuals/Marketplace.cs").read_text(encoding="utf-8")
        self.assertIn("html, body { height: 100%; background: transparent; color-scheme: normal; }", page)
        self.assertIn("Background = Avalonia.Media.Brushes.Transparent,", host)
