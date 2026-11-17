# Experimental Avalonia launcher (step a)

An opt-in app alongside the shipping Windows and web launchers, not a replacement.
Existing release workflows, installers, shortcuts and frontend tests are unchanged.

Run with the .NET 8 SDK:

```
dotnet run --project OpenSmash.Avalonia/OpenSmash.csproj
```

Publish an experimental package:

```
python tools/publish_avalonia.py --rid win-x64
```

Use linux-x64 or osx-arm64 on those platforms. Packages go under dist/avalonia/.
The executable is OpenSmash.Avalonia, never the shipping OpenSmash executable.
Install it explicitly with python tools/build_avalonia.py --rid win-x64.

This is the earlier prototype, not a claim of parity. Follow-ups must port
#383/#420/#427 update compatibility, #431 problem reporting, #432 leaderboard
pictures, #435 renderer_menu compatibility, and #438 live shared settings.
The app compiles the existing SettingDefaults source instead of duplicating it.
Avoid editing settings with both the prototype and game until the live refresh
port lands. Linux/macOS hardware, real-controller, Discord and old-install
update checks remain necessary before any switch of production defaults.

The layout helper only adds missing compatibility aliases. It never deletes
old entrypoints or rewrites existing installer backends. Existing launchers
and all their tests remain. This workflow uploads artifacts, not releases.
