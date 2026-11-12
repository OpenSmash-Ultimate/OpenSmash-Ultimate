# UI languages

The Windows launcher (including setup and its embedded mod browser) and the current
external game menu (`runtime/gpu_menu.c`) share the UTF-8 catalogs in `locales/`.
English is the source and fallback language. Simplified Chinese, Japanese, Korean,
Spanish, French and German catalogs are supplied; coverage is described below.
The staged `ssbu-recomp/` UI is a separate upstream implementation and is not the
menu built by this project's runtime.

Choose **Settings > Display > Interface language** in the launcher, or
**Game > Interface language** in the external menu. The choice takes effect in
that interface immediately and is saved as `ui_language` in the same `display.ini`
the project already reads (under `config/` after configuration migration).
The other interface reads it on its next start. `auto` follows the system UI
language. `SSBU_UI_LANGUAGE=en` or `SSBU_UI_LANGUAGE=zh-CN` overrides the file for
one process; `SSBU_UI_LANGUAGE=auto` explicitly follows the system instead.
The game's existing `language` / `SSBU_LANGUAGE` settings are independent.

## Editing or adding messages

A message's English source is its ID, as in gettext. UI metadata and settings
values stay in their original form. The drawing helpers translate before
measuring, wrapping or drawing; the menu translates `printf` templates before
formatting them. Unknown IDs and unsupported languages return English. There is
no fuzzy matching, machine translation or online service.

For a new static launcher label, use the existing `Theme` helpers normally:

```csharp
Theme.DrawText(g, "New option", font, color, x, y, width, height);
```

Add the translation in `locales/zh-CN.json`:

```json
"New option": "新选项"
```

Use a whole template for dynamic text. Keep variables out of the ID:

```csharp
UiLocale.Format("Downloaded {0} files", count)
// Or a C# interpolated string converted to the same numbered-placeholder ID:
UiLocale.Interpolate($"Downloaded {count} files")
```

The corresponding translation is `"Downloaded {0} files": "已下载 {0} 个文件"`.
Keep diagnostic/process helpers independent of the UI adapter. Translate their
status at the presentation boundary. Use `localize: false` when drawing player
names, mod titles or other user/remote content with `Theme.DrawText`; paths have
their existing `DrawPath` renderer. Never translate persisted option values,
configuration keys, network values, controller IDs, file paths or command arguments.

For the native menu, `add` translates static labels/help, `help_set` translates
its format, and a dynamic `snprintf` uses `ui_text` on the format **before** the
arguments are substituted:

```c
snprintf(row->value, sizeof row->value, ui_text("Downloaded %d files"), count);
```

Its translation is `"Downloaded %d files": "已下载 %d 个文件"`. Keep C format
arguments in the same order. A complete sentence avoids joining translated
fragments. For text whose meaning depends on plural quantity, select complete
source messages in the caller. The catalog adapter does not
implement a general plural rules engine or right-to-left layout.

The embedded browser's `UiLocale.Web.js` localizes its own buttons, filters and
status elements from the same catalog. Mod titles, authors and descriptions
stay verbatim. Use `window.__opensmashFormat("Page {0} / {1}", page, pages)` for
its dynamic UI text. No translation data belongs in JavaScript or drawing code.
Uppercase versions of plain UI IDs are generated automatically; translators do
not need to keep uppercase copies in sync. An explicit uppercase entry wins.

After editing any catalog:

```sh
python tools/localization.py
python tools/localization.py --check
```

Commit the JSON and `runtime/ui_catalog.generated.h` together. Never edit the
generated header. The check rejects duplicate JSON keys, changed `printf`
argument sequences, changed .NET placeholder names/formats, and stale output.
It also runs in the existing source-only CI pipeline. The tool uses Python's
standard library; the launcher and runtime acquire no new package dependency.

## Adding a language

Add `locales/<code>.json` with a native display name and the translated messages:

```json
{
  "name": "Français",
  "messages": {
    "Settings": "Paramètres",
    "Interface language": "Langue de l'interface"
  }
}
```

Run the two commands above and rebuild. Both selectors discover the catalog
automatically. Missing entries can be supplied later and fall back to English.
Exact locale tags win, then a supported parent language (`zh-Hans` to `zh-CN`,
`en-GB` to `en`), then English. Regional languages should use standard BCP 47
language tags; underscores and POSIX `.UTF-8`/`@...` suffixes are accepted.

## Fonts and verification

The external menu decodes UTF-8 and uses display-column widths for measuring,
truncation and wrapping. ASCII glyphs occupy one atlas cell; catalog Unicode
glyphs reserve two. Windows draws the atlas with GDI using system fonts (Consolas
Microsoft YaHei UI, Yu Gothic UI for Japanese kana, and Malgun Gothic for Korean). Linux's existing Pillow font generator uses fontconfig's
Chinese, Japanese and Korean fallback faces; system CJK fonts such as Noto Sans CJK are needed for those languages. macOS's existing Core Text generator selects fallback fonts per
Unicode string. The catalog determines the glyph inventory and invalidates the
font build cache. Only source and codepoints are committed: no generated glyph
bitmaps, system fonts, console fonts or game-derived assets.

Source-only checks, with no game files:

```sh
python -m unittest discover -s tests -p test_ui_localization.py -v
dotnet run --project tests/launcher_localization/LauncherLocalizationTests.csproj -c Release
python tools/dev/runtime_compile_check.py --tree <temporary-folder> --game-dll
```

The tests cover catalog safety, adding a language without adapter changes,
UTF-8 layout, production-menu switching/persistence, user text, Windows atlas
coverage and quad widths, launcher settings, and the embedded browser with
local synthetic responses when WebView2 is installed. The launcher test accepts
an output directory for English/Chinese settings screenshots at 100% and 150%.
These tests do not establish in-match rendering, Vulkan validation-layer
cleanliness or frame-time performance. Measure those on actual hardware when
validating a release, as required by `CONTRIBUTING.md`.

## Shipped languages and translation coverage

The language selector includes English, 简体中文, 日本語, 한국어, Español, Français and Deutsch.
Chinese has the broader initial catalog. Japanese, Korean, Spanish, French and German each
start with more than 200 messages for navigation, common actions, display, audio, controllers,
online options and language selection. Detailed setup instructions and uncommon errors
that have no translation remain in English. These are initial AI-assisted translations,
and native-speaker review is welcome; they are not claimed as complete translations.

Both selectors discover all seven catalogs automatically. Region variants such as
fr-CA, es-MX, de-AT, ja-JP and ko-KR use the matching parent language. UI language
selection does not change the game's language, saved network values or mod titles.
