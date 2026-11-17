"""The sort names the launcher's Browse Mods page (OpenSmash.Launcher/MarketplaceHtml.html) can send to GameBanana as
_sSort: the options of its Sort list and the sort it starts with. GameBanana answers a name it does not know with
HTTP 400 (UNKNOWN_SORT, "Sort alias not recognized"), and the page then shows "Couldn't load mods" for that sort.
KNOWN are the names seen answered with HTTP 200 by gamebanana.com/apiv12/Mod/Index for game 6498 on 2026-11-17
(the page's own request, sent with curl). Text only: runs anywhere, asks GameBanana nothing."""
from pathlib import Path
import re
import unittest

PAGE = Path(__file__).resolve().parent.parent / 'OpenSmash.Launcher' / 'MarketplaceHtml.html'
KNOWN = {'Generic_Newest', 'Generic_MostLiked', 'Generic_MostDownloaded', 'Generic_MostViewed',
         'Generic_LatestUpdated'}


def page_sorts(text):
    """(the Sort list's option values in order, the sort the page's state starts with)."""
    block = re.search(r'<select id="sort"[^>]*>(.*?)</select>', text, re.S).group(1)
    start = re.search(r'const state = \{.*?\bsort: "([^"]*)"', text, re.S).group(1)
    return re.findall(r'<option value="([^"]*)"', block), start


class Sorts(unittest.TestCase):
    def test_every_sort_the_page_can_send_is_one_gamebanana_knows(self):
        options, start = page_sorts(PAGE.read_text(encoding='utf-8-sig'))
        self.assertTrue(options, 'the Sort list has no options: the page changed, read it again')
        self.assertEqual([s for s in options if s not in KNOWN], [])
        self.assertIn(start, options)


if __name__ == '__main__':
    unittest.main()
