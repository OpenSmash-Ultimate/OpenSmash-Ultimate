# Controlled integration with main

Reviewed upstream: `df947945a4be3b78ad3be717ad30aba8ce86975f` (2026-11-12).
Original export base: `c775c2e31a12c2d03b0b199e691455c1e553b2ce`.
This integrates 56 upstream commits and resolves 25 overlapping paths.

## Resolution choices

- Keep the shared Avalonia launcher, three platform builders, portable project discovery,
  internal `tools/` entry points, bootstrap migration, marketplace and native artwork.
- Retain upstream runtime, GPU, input, network/arena, mod extraction, plugin and build fixes.
- Port newer Windows frontend behavior into shared controllers instead of restoring the
  deleted Forms source: settings preservation, renderer override cleanup, SPIR-V readiness,
  Python/venv preparation, vanilla-game rebuilds and resumable source/game/launcher updates.
- Keep the shared launcher as the platform shortcut target. Upstream's browser launcher
  is superseded; its native/backend regression checks are retained separately.
- Combine upstream build-readiness exclusions with the shared launcher exclusions.
- Keep release and CI workflows for the shared launcher and upstream runtime workflows.
- On Python before 3.14, reject incomplete Zstandard shader frames, matching Python 3.14's
  behavior. The pre-existing truncated-frame test exposed this difference during validation.

## Validation scope

Desktop regression tests exercise settings changed externally while open, SPIR-V markers,
vanilla readiness, inherited renderer cleanup, update fingerprinting, failed rebuild retry,
portable bootstrap/relocation, mods/plugins and original artwork. New upstream backend,
shader, plugin binding, installer and runtime tests are included in the Python checks.
Native macOS/Linux gameplay and a real player's game recompilation are not run on this
Windows host. Existing game installs are not overwritten with newer runtime/backend files
because those must be rebuilt together (including the changed plugin binding ABI).
