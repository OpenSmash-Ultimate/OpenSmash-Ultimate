# Original launcher fonts

The shared canvas pins the open-source font files used by the reference Windows
launcher, so font discovery on another desktop cannot change the layout.

- `Inter-Regular.ttf` and `Inter-Bold.ttf`: Inter 3.012, upstream git revision
  `06b166889`, copyright 2020 The Inter Project Authors. SIL Open Font License 1.1;
  see `../Inter-LICENSE.txt` and https://github.com/rsms/inter.
- `CascadiaMono.ttf`: Cascadia Mono 2102.025, Microsoft Corporation. See the
  accompanying `CascadiaMono-LICENSE.txt`, from upstream release `v2102.25`:
  https://github.com/microsoft/cascadia-code.
- Anton and the emblem continue to come from `OpenSmash.Launcher/Assets`.

The font binaries are unmodified. The copies were matched to their embedded font
version and licensing metadata. The GDI+ renderer is Windows-only; other systems
use Skia with these same fonts and the original drawing coordinates.
