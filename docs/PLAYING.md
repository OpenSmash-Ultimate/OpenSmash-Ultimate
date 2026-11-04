# Playing

[← README](../README.md) · [Setup guide](BUILDING.md) · [Multiplayer](MULTIPLAYER.md) · [DLC and unlocks](DLC-AND-UNLOCKS.md)

Everything about playing once the game is built: how to start it, the controls, the settings, and where WindowSmash keeps your save, screenshots and problem reports.

## Contents

- [Starting the game](#starting-the-game)
- [Settings](#settings)
- [Controls](#controls)
- [Controllers](#controllers)
- [Window and display](#window-and-display)
- [Shaders and first-time stutter](#shaders-and-first-time-stutter)
- [Low frame rate](#low-frame-rate)
- [Your own Miis](#your-own-miis)
- [Snapshots and the Vault's album](#snapshots-and-the-vaults-album)
- [Where your files are](#where-your-files-are)
- [Problem reports](#problem-reports)

## Starting the game

Everything starts from a launcher. `Play SSBU.bat` and `Install WindowSmash.bat` are in the WindowSmash folder; the others are in its `launchers` folder. Double-click the one you want:

| Launcher | What it does |
| :--- | :--- |
| **`Play SSBU.bat`** | Opens the WindowSmash window (below), then starts the game and joins the online lobby, so Local Wireless shows everyone's rooms. If the lobby doesn't answer, the game plays offline. |
| **`Play SSBU.bat`** with <kbd>Shift</kbd> held | Opens the WindowSmash window even when you've ticked **Start the game directly next time**. |
| **`launchers\Play SSBU Offline.bat`** | Starts the game without going online. |
| **`launchers\Host SSBU Netplay.bat`** | Starts the game as the host of a private room ([multiplayer guide](MULTIPLAYER.md#private-room)). |
| **`launchers\Join SSBU Netplay.bat`** | Starts the game and joins someone's private room. |
| **`launchers\Unlock All Fighters.bat`** | Unlocks every base-game fighter on your save. Close the game first ([DLC and unlocks](DLC-AND-UNLOCKS.md#unlock-every-base-game-fighter)). |
| **`launchers\Prepare Shaders.bat`** | Prepares all of the game's shaders ahead of time ([below](#shaders-and-first-time-stutter)). |
| **`Install WindowSmash.bat`** | Sets everything up and builds the game ([setup guide](BUILDING.md#install-in-one-go)). Run it again to get the newest WindowSmash and build it. |
| **`launchers\Build WindowSmash.bat`** | Builds the game from your files, when the tools are already installed ([setup guide](BUILDING.md#shortcut-build-windowsmashbat)). |
| **`launchers\Configure Game Data.bat`** | Points WindowSmash at your game data and fonts ([setup guide, step 10](BUILDING.md#step-10-connect-your-game-data)). |

`Play SSBU.bat` first opens a small window that shows the game version (13.0.5) and changes the most used settings: your online name and lobby, the window and frame pacing, the render scale, the graphics card, and which player the keyboard is. **Play** saves what you changed and starts the game; **Close** changes nothing. It edits the same files as the in-game [settings](#settings), keeping their comments. Tick **Start the game directly next time** to skip the window; hold <kbd>Shift</kbd> while you start `Play SSBU.bat` to see it again.

The first time you play online, the game picks a name for you, such as `Player1234`, and shows it at the bottom of the screen. To change it, open the [settings](#settings) and go to **Online**.

## Settings

Press <kbd>Esc</kbd> in the game to open the settings. On a controller, press its **Home** button (the PS or Xbox button), or hold **−** and **+** together for a second. Close the settings the same way, or with **B**.

| Page | What you can change |
| :--- | :--- |
| **Display** | Window or full screen, frame pacing, how many frames the graphics card works ahead, the mouse pointer, the frame rate in the corner, the window's size, the render scale, the graphics card. |
| **Controllers** | For each connected controller: which player it is, face buttons by position or by label, stick dead zone, rumble, how far a trigger is pulled for ZL / ZR, and which control each button is (**Buttons...**). For the keyboard: which player it adds to, and which key is each button (**Keyboard keys...**: choose a row, press the new key; <kbd>Delete</kbd> or **Y** puts its default back). Also **Rumble** on or off for every controller, and whether controllers work while another window is in front. |
| **Online** | Public lobby, a private lobby code, your own server, or offline; your name; direct connections; the netplay version ([multiplayer guide](MULTIPLAYER.md)). |
| **Game** | Restart or quit the game, the keys, and the game version. |
| **Lobby** | The online lobby's rooms, the ones you can't join too (and why), and matchmaking. With the cursor on a room, its game version and its mods, each marked as you have it. They need netplay version 3, which is being tried out ([multiplayer guide](MULTIPLAYER.md#room-browser-and-matchmaking-being-tried-out)). |
| **Mods** | Each installed mod on or off, **ARCropolis code patches** on or off, and **Turn every mod off** ([below](#mods-on-and-off)). |

- **Move** with the arrow keys or the D-pad. **Change** a setting with left and right. **Select** with <kbd>Enter</kbd> or **A**. **Switch pages** with <kbd>Tab</kbd>, the number keys <kbd>1</kbd>–<kbd>6</kbd>, or **L** / **R**.
- The game **keeps running** behind the settings, but it doesn't see your buttons or keys while they're open. In a match, pause first. In a netplay room, the other players keep playing.
- Every change is **saved at once**. Settings marked **now** take effect straight away. Settings marked **next start** take effect the next time the game starts: **Game → Restart the game** does that for you. A match in progress is lost when you restart.
- To type a name or a code, use the keyboard, or pick the letters with a controller: **A** types, **B** deletes, **+** finishes.

### Mods on and off

**To install mods** (skins, stages, music, movesets and new characters from GameBanana, or a mod's `.zip` you have), close the game and double-click **`launchers\WindowSmash Mods.bat`**: the mod manager finds GameBanana's Smash Ultimate mods, installs them, turns them on and off and applies them to the game. Mods made for ARCropolis and Skyline plugins work; the runtime provides ARCropolis, Smashline 2, lib_paramconfig and the CSK Collection itself, so don't install those. How plugins run: [Mods and Skyline plugins](SKYLINE.md).

In the OpenSmash launcher's **Mods** view, **Check** lists the files two turned-on mods both change (the game reads the later one's) and the GameBanana mods that have a newer file, and installs those on a yes; **Share** saves your mods as a list file, or installs the mods of a list a friend sent you (from GameBanana; mods that aren't there are named). These are TRLLZR's mod manager ideas (NOTICE.md); `tools/mods.py conflicts`, `updates`, `export-list` and `import-list` do the same from a terminal.

The **Mods** page lists every installed mod with its own switch: the same switches as the launcher's mod list and the Mods app's (they share `mods\mods.json`). A change counts the next time the game starts: choose **Restart now**, and the game applies the mods before it reads them. Online, players meet only when they run the same mods, so turn off what the others don't have; the **Lobby** page says, room by room, which mods differ.

**ARCropolis code patches** are the fixes ARCropolis makes to the game's code for costume slots that mods add (c08 and up). They have a switch of their own: **auto** (the default) turns them on while a mod runs and off otherwise, so the game with no mod runs its own code, as an unmodded game. They count when the game starts, and players online need the same setting. `SSBU_CODE_PATCHES=1` or `0` sets them for one start (tests).

The settings are kept in the same files as always: `display.ini`, `controllers.ini` and the `online_*.txt` files in the WindowSmash folder. You can still edit those files by hand, and the settings keep your comments in them. Some things are only in the files, such as settings for a whole kind of controller ([below](#controllersini)).

## Controls

The keyboard plays as player 1, together with player 1's pad if there is one. These are its default keys; change any of them in the [settings](#settings) (**Controllers → Keyboard keys...**) or in the `[keyboard]` section of [`controllers.ini`](#controllersini):

| Switch button | Keyboard |
| :--- | :--- |
| Left stick | Arrow keys |
| A / B / X / Y | <kbd>Z</kbd> / <kbd>X</kbd> / <kbd>C</kbd> / <kbd>V</kbd> |
| L / R | <kbd>A</kbd> / <kbd>S</kbd> |
| ZL / ZR | <kbd>Q</kbd> / <kbd>W</kbd> |
| + / − | <kbd>Enter</kbd> / <kbd>Backspace</kbd> |
| Right stick | <kbd>I</kbd> <kbd>J</kbd> <kbd>K</kbd> <kbd>L</kbd> |
| Walk (left stick part way) | Hold <kbd>Shift</kbd> with the arrow keys |

An arrow key pushes the stick all the way at once, which the game takes as a flick: your fighter dashes, and down drops through a platform. Hold <kbd>Shift</kbd> (the `WALK` key) to push it part way instead: sideways walks and gives tilt attacks with A, down crouches on a platform without dropping through.

| Key | What it does |
| :--- | :--- |
| <kbd>Esc</kbd> | Opens or closes the [settings](#settings). |
| <kbd>F1</kbd> | Shows or hides the overlay: frame rate and, online, the connection. |
| <kbd>F2</kbd> | Shows more details, including the connected pads. |
| <kbd>F3</kbd> | Takes a screenshot. |
| <kbd>F11</kbd> or <kbd>Alt</kbd>+<kbd>Enter</kbd> | Switches between a window and full screen. |

<details>
<summary><strong>Keys for troubleshooting</strong></summary>

<br>

| Key | What it does |
| :--- | :--- |
| <kbd>F4</kbd> | Writes one frame's drawing commands to the log. |
| <kbd>F5</kbd> | Turns depth sampling on or off. |
| <kbd>F6</kbd> | Records a 10-second profile into `build\profile_*.txt`. |

</details>

## Controllers

Up to **eight players** can use pads at once: Xbox, PlayStation, Switch Pro, GameCube, and most other controllers.

- Each pad that connects becomes the **next free player**. A pad that's unplugged and plugged back in gets its player back.
- Buttons go by **position**, as on a Switch controller. On an Xbox controller, the right-hand button (**B**) is the Switch's **A**, and the bottom one (**A**) is the Switch's **B**. To use the printed letters instead, or to change any button, open the [settings](#settings) and go to **Controllers**.
- The settings' **Controllers** page, or <kbd>F2</kbd> in the game, shows which pads WindowSmash found and which player each one is.

> [!NOTE]
> PlayStation, Switch Pro and most other pads need `SDL3.dll` next to `build\ssbu.exe`. [Step 12 of the setup guide](BUILDING.md#step-12-optional-support-more-kinds-of-controllers) adds it. Without it, only Xbox (XInput) pads work, as players 1–4.

### GameCube controllers

These work, each port or controller as its own player:

- **Nintendo's GameCube controller adapter** (the one for Wii U and Switch), or another brand's adapter (Mayflash and others) set to its **Wii U** mode. Windows' own driver and the WinUSB driver Dolphin's setup installs with Zadig both work.
- Another brand's adapter in its **PC** mode.
- The **Nintendo Switch Online GameCube controller** (the one for Switch 2), on a **USB cable**. Its Bluetooth doesn't work with WindowSmash.

They play as GameCube controllers, as they do on a Switch:

- The game uses its own **GameCube controller** controls, which you can change in the game's **Options → Controls**.
- The buttons go by the letters printed on them. **L** and **R** are analog, so the game reads how far they're pulled, and **Z** works as on the console.
- Rumble works when the adapter's second plug (the grey one on Nintendo's adapter) is in a USB port too. WaveBird controllers have no rumble.

> [!NOTE]
> **The Switch Online GameCube controller needs one more step on some PCs.** It only starts sending its buttons after the game talks to its second USB "interface", and Windows may not give that interface a driver. If the log (`build\play.log`) says *its interface 1 has no WinUSB driver*:
>
> 1. Plug the controller in with its cable and open [Zadig](https://zadig.akeo.ie).
> 2. Choose **Options → List All Devices**.
> 3. Pick the controller's **Interface 1**: the USB ID shows `057E 2073 01`. Not Interface 0, which Windows needs for the buttons.
> 4. Choose **WinUSB** as the driver and click **Replace Driver** (or **Install Driver**).
>
> Start the game again. The log then says *Switch 2 GameCube controller started*.

The log names what it finds, on lines that start with `[input]`: an adapter (*GameCube adapter found*), each controller that becomes a player, and anything that stopped one from working.

Any other pad can use the game's GameCube controls too: set `style = gamecube` for it in `controllers.ini` (below). Its triggers become L and R, and its shoulder buttons become Z.

### controllers.ini

The first time the game starts, it writes **`controllers.ini`** into the WindowSmash folder. The [settings](#settings) change it for you; you can also open it in Notepad (**Controllers → Open controllers.ini** opens it). Every setting is explained inside the file. Changes made by hand apply the next time the game starts, and deleting the file brings back the defaults.

| Setting | What it does | Default |
| :--- | :--- | :--- |
| `keyboard_player` | Which player the keyboard adds to: 1–8, or 0 for no keyboard. | `1` |
| `background` | `1`: pads keep working while the game window isn't in front. `0`: only while it is. | `1` |
| `layout` | `positional`: buttons by position, as on a Switch pad. `labels`: by the letter printed on the pad (PlayStation pads stay positional). | `positional` |
| `style` | `auto`: GameCube controllers play as GameCube controllers, other pads as Pro Controllers. `gamecube` or `pro`: this pad plays as that kind of controller. | `auto` |
| `deadzone` | How much of the stick's travel around the centre is ignored, 0–50 percent. | `0` |
| `rumble` | Rumble strength, 0–100 percent. `0` turns it off. (In `[general]`: `0` turns rumble off for every controller, `1` leaves each at its own strength.) | `100` (`1` in `[general]`) |
| `trigger_threshold` | How far a trigger must be pulled to press ZL or ZR, 1–99 percent. (On a GameCube controller, the game itself decides how far L and R are pulled.) | `25` |
| *remaps* | `<Switch button> = <pad control>`, for example `ZR = rightshoulder`. The file lists every name. | |

The file has these sections:

- **`[general]`** holds `keyboard_player`, `background` and `rumble` (on or off for every controller).
- **`[keyboard]`**'s `movement = ssf2` makes the arrow keys move as in Super Smash Flash 2: a key walks, the same key again quickly dashes, down crouches and again quickly drops through a platform, A just after a direction key is a smash attack, and up is a full hop (held) or a short hop (tapped), with A or B in it an up smash or up special; the d-pad's up key taunts in the direction held. `movement = full` (the default) pushes the stick fully at once. `SSBU_KEYBOARD_MOVEMENT=ssf2` or `full` overrides it.
- **`[keyboard]`** holds the keyboard's keys: `<Switch button> = <key>`, for example `A = Space` or `LSTICK_LEFT = A`. The buttons are `A B X Y L R ZL ZR PLUS MINUS LSTICK RSTICK` (the last two: the sticks' clicks) and the d-pad's `UP DOWN LEFT RIGHT`; the sticks' directions are `LSTICK_LEFT LSTICK_UP LSTICK_RIGHT LSTICK_DOWN` and the same with `RSTICK_`; `WALK` (default `Shift`) is held to push the left stick part way. A key is a letter or digit, `Left Up Right Down Enter Backspace Space Tab Shift Ctrl Alt`, `F1`–`F12`, `Num0`–`Num9` (the number pad), `0x` and a Windows virtual-key code, or `none`; list several with commas (`A = Z, Space`). Removing a line puts that key's default back. An ssbu-recomp `[keyboard]` section carries over as it is (its `STICK_L` / `STICK_R` are `LSTICK` / `RSTICK`); its `movement = ssf2` is kept in the file but has no effect here yet.
- **`[players]`** reserves a player for one pad, for example `2 = DualSense Wireless Controller` (the settings' **Player** row writes the pad's GUID there). The log names every pad as it connects, with its type and GUID.
- **`[device default]`** applies to every pad.
- **`[device ps5]`**, **`[device <name>]`** or **`[device <GUID>]`** applies to one kind of pad, or one pad. Types include `xbox360`, `xboxone`, `ps4`, `ps5`, `switchpro`, `joyconpair` and `gamecube`. The settings write a controller's changes under `[device <its name>]`, or under the section it already takes its settings from.

For example, to make every pad use the printed letters, turn rumble down on a PlayStation 5 pad, and give Xbox One pads the game's GameCube controls:

```ini
[device default]
layout = labels

[device ps5]
rumble = 60

[device xboxone]
style = gamecube
```

And to move with <kbd>W</kbd> <kbd>A</kbd> <kbd>S</kbd> <kbd>D</kbd> and jump with <kbd>Space</kbd>, with L and R (on <kbd>A</kbd> and <kbd>S</kbd> by default) and ZR (on <kbd>W</kbd>) moved out of the way:

```ini
[keyboard]
LSTICK_LEFT = A
LSTICK_UP = W
LSTICK_RIGHT = D
LSTICK_DOWN = S
X = Space
L = Shift
R = Ctrl
ZR = E
```

### Steam and Steam Input

To start the game from Steam (Big Picture, or Steam Input for a controller the game doesn't know):

1. In Steam, choose **Games → Add a Non-Steam Game to My Library**, tick any program and click **Add Selected Programs**.
2. Right-click it in your library and choose **Properties**. Set:
   - **Target:** the game itself, with quotes: `"C:\WindowSmash\build\ssbu.exe"`
   - **Start In:** the WindowSmash folder: `"C:\WindowSmash"`
   - **Launch options:** `dump\modules --status 30 --online` (leave out `--online` to play offline)
3. Rename it to *Super Smash Bros. Ultimate* if you like.

Use your own folder in place of `C:\WindowSmash`. Started this way the game skips the small WindowSmash window, and its log is not written to `build\play.log`. Steam's keyboard settings can't change the keyboard's keys: change them in the game ([Controls](#controls)).

On a Steam Deck or another Linux PC, add `run.sh` the same way, with **Start In** set to the WindowSmash folder ([Linux](LINUX.md)).

## Window and display

<kbd>F11</kbd> or <kbd>Alt</kbd>+<kbd>Enter</kbd> switches between a window and full screen at any time. The game draws its picture at 1920 × 1080 and fits it into the window, always at 16:9, with black bars when the window has another shape. `render_scale` (below) draws it bigger for a sharper picture on a bigger screen. Screenshots keep the whole picture at the size it was drawn.

The first start also writes **`display.ini`**, which sets how the game starts. The settings' **Display** page changes it too:

| Setting | Values | Default |
| :--- | :--- | :--- |
| `window` | `windowed`: a window that keeps 16:9 as you resize it. `borderless`: covers its whole monitor. `fullscreen`: borderless and always on top; the monitor keeps its resolution. | `windowed` |
| `width`, `height` | The window's inside size at 100% display scaling. | `1280`, `720` |
| `pacing` | `clock`: a steady 60 frames a second. `vrr`: for G-Sync or FreeSync monitors, each frame shown as soon as it's ready; best with `borderless`. `display`: follows a 60, 120 or 240 Hz monitor's own refresh. | `clock` |
| `latency` | How many frames the graphics card may work ahead, 1–3. Lower is more responsive. | `3` |
| `low_latency` | `1`: the game starts each frame as late as it can and still make the next refresh, so it reads your controller later: up to most of a frame less input delay. If a frame misses its refresh it turns itself off for a second. | `1` |
| `hide_cursor` | `1` hides the mouse pointer after two seconds without movement. | `1` |
| `language` | The game's language: `auto` (the language Windows shows its menus in), or one of `ja`, `en-US`, `en-GB`, `fr`, `fr-CA`, `de`, `it`, `es`, `es-419`, `zh-Hans`, `zh-Hant`, `ko`, `nl`, `ru`. A language the game doesn't have plays in English. Chinese, Japanese and Korean text needs your console's firmware fonts. Read when the game starts; the settings menu's Game page changes it. `SSBU_LANGUAGE` overrides it. | `auto` |
| `volume` | The game's whole sound, from `0` (silent) to `100`, over the game's own sound settings. The settings menu's Game page changes it while you play. `SSBU_VOLUME` overrides it. | `100` |
| `overlay` | `1` shows the frame rate in the corner from the start (<kbd>F1</kbd> shows or hides it while playing). | `1` |
| `gpu` | Which graphics card draws the game. `auto`: the fastest; on a laptop with two, the NVIDIA or AMD one rather than the processor's own. Or part of a card's name, such as `nvidia`, `radeon` or `intel`. `build\play.log` lists the cards found (a `[gfx] graphics cards` line). | `auto` |
| `render_scale` | How many times bigger than 1920 × 1080 the game draws its picture, in each direction: a number from `0.5` to `4`, or `auto`. `auto` matches the picture to the screen when the game starts, for example 1.33 (2560 × 1440) on a 1440p monitor with `window = borderless`; it never goes below 1. A picture bigger than the window is shrunk to fit, which smooths edges (`2` on a 1440p monitor). Each step up costs the graphics card more: `2` draws four times the pixels and needs about 200 MB more graphics memory. | `1` |
| `renderer` | `d3d11`: Direct3D 11, the renderer the game is played and tested with. `vulkan`: the Vulkan renderer, in development: until it can draw the game, the game checks Vulkan on your PC at its start, writes what it found in `build\play.log` (a `[gfx] renderer = vulkan` line), and draws with Direct3D 11 as before. The settings' Display page has it as **Renderer**; `SSBU_RENDERER` overrides it. Read when the game starts. | `d3d11` |

A `display.ini` written by an older WindowSmash has no `gpu`, `render_scale` or `renderer` line; the game then uses `auto`, `1` and `d3d11`. Add the line yourself to use it, or delete `display.ini` to get a new one with every setting. `render_scale` is read when the game starts: change it, then start the game again.

## Shaders and first-time stutter

The game's graphics programs (*shaders*) are prepared the first time each one is used, so a new effect can show a moment late, or stutter, the first time you see it. WindowSmash keeps every prepared shader in `dump\shadercache\` and loads them in the background each time the game starts, so each stutter happens only once.

To prepare them all before playing, double-click **`launchers\Prepare Shaders.bat`**. It runs at low priority so the PC stays usable, and shows a progress bar with the time left. A first run compiles about 40,000 shaders: about half an hour on a 16-thread processor, an hour or two on a 4- to 8-thread one. You can close it at any time and start it again later; what's done stays done, so a later run takes minutes. It needs Python and Visual Studio, as for [building](BUILDING.md).

## Low frame rate

If matches run slowly on a PC that should manage them (press <kbd>F1</kbd> for the frame rate), check these first:

- **The PC is below what the game needs.** It needs about what an emulator needs to run Smash well: if Smash struggles in an emulator on your PC, it struggles here too. A dual-core laptop chip, or an old entry-level graphics card, is not enough; processor graphics (Intel UHD / Iris Xe, AMD Vega) play, but slower, and stutter more the first time each effect shows ([above](#shaders-and-first-time-stutter)); keep `render_scale` at its default there.
- **A laptop is drawing with its processor's graphics.** With two graphics chips, the game takes the NVIDIA or AMD one by itself. Check the `[gfx] drawing with` line near the top of `build\play.log`, and see `gpu` in [`display.ini`](#window-and-display).
- **Prepare Shaders is still running.** It uses most of the processor; let it finish, or close it while you play.
- **NVIDIA's shader cache is off.** In NVIDIA Control Panel → *Manage 3D settings*, set **Shader Cache Size** to *Driver default* (not *Disabled*). Without it, the graphics driver redoes its part of every shader each time the game starts.
- **Windows uses a slow system timer.** Some "gaming tweaks" set Windows to time everything with an old, slow timer (the HPET). WindowSmash notices and works around it, and says so near the top of `build\play.log`: a `[clock]` line ending in *slow*. Other programs stay slower, though; to undo the tweak, run `bcdedit /deletevalue useplatformclock` in a Command Prompt opened as administrator, then restart the PC.

## Your own Miis

The game lists your own Miis wherever it lists Miis, such as **Games & More → Mii Fighters**, after its six default ones. WindowSmash has no Mii editor of its own, so you make them somewhere else and import them:

1. Make your Miis in an emulator's Mii editor (Ryujinx's **Mii Edit Applet**, yuzu's or its forks' **Open Mii Editor**, in their menus), or on Mii Studio, or bring a Mii file.
2. Double-click **`launchers\Import Miis.bat`** (on a Mac, `launchers/Import Miis.command`). It imports the Miis of every emulator it finds on this PC, then lets you paste Mii Studio links: on Mii Studio, right-click your Mii's picture, choose **Copy image address**, and paste it. To import Mii files instead, drop them on `Import Miis.bat`: `.charinfo`, `.nfsd` and `.nfcd` from a Switch, `.ffsd` and `.cfsd` from a 3DS or Wii U (not QR codes), or an emulator's `MiiDatabase.dat`.
3. Restart the game.

Each Mii becomes one file in the `miis` folder, `miis\<name>.charinfo`. To give a Mii to a friend, send them that file to put in their own `miis` folder. To remove one, delete its file. The game reads at most 100. A Mii Fighter you make from one of your Miis is kept in your save, so it stays even after you remove the Mii's file. From PowerShell, `python tools/miis.py list` lists them and `python tools/miis.py import --help` shows every option.

## Snapshots and the Vault's album

Pictures you save in the game (pause a battle and take a snapshot) are kept in the **`album`** folder, and the game lists them in its **Vault** album as it does on the console. Each picture is a `.bmp` file named after when it was taken, `<date>_<time>_<number>.bmp`, so you can open or copy it with any picture viewer; a `.userdata.bin` file beside it holds what the game keeps with the picture. To remove a picture, delete both files while the game is closed. An album folder from ssbu-recomp (its `userdata\album`) can be copied over as it is.

Videos made from replays are not kept yet: the game can't make one.

## Where your files are

All of these are in the WindowSmash folder:

| What | Where |
| :--- | :--- |
| Your save | `save\` |
| Your own Miis | `miis\` ([above](#your-own-miis)) |
| Screenshots (<kbd>F3</kbd>) | `screenshots\` |
| Pictures saved in the game (the Vault) | `album\` ([above](#snapshots-and-the-vaults-album)) |
| The game's log | `build\play.log` |
| Crash and freeze reports | `reports\` |
| Controller and window settings | `controllers.ini`, `display.ini` |
| Your online name, lobby, direct connections and netplay version | `online_name.txt`, `online_server.txt`, `online_direct.txt`, `online_protocol.txt` |
| Your multiplayer identity | `netplay_id.txt`. Don't copy it to another PC: two PCs with the same one look like one player. |

Your save is written as the console writes it: what the game saves reaches the `save` folder all at once, when the game finishes saving. A crash or a power cut while it saves leaves your last complete save, never half of one. While the game runs, the `save` folder also holds the changes not saved yet, in a folder ending in `.work.` and a number, beside your save's own folder, and for a moment while it saves, a `.journal.` file and a `.commit` file. On Linux and macOS a `.lock` file stays there too. Leave them be: the next start finishes a save that was cut short and clears the rest.

If `build\play.log` says *could not finish applying a save*, your PC would not let the game change a file in the `save` folder: another program holds it open, or the folder belongs to another user (after running the game with `sudo`, say), or your save's own folder is a link to another drive. Nothing is lost, but the game can't save, or load its save, until that is fixed. Close the game, fix it (make the `save` folder and everything in it yours; to keep your save on another drive, link the whole `save` folder, not the folder inside it), then start the game: it finishes the save first.

The game's own files (`dump\`, with the shader cache) and the compiled game (`build\`) take tens of GB. To put them on another drive, close the game and the launcher, then run `python tools\data_location.py --move D:\OpenSmash` (any empty folder). They move there, and a link stays in their place, so nothing else changes. `python tools\data_location.py` says where they are; the same `--move` with the WindowSmash folder brings them back.

> [!TIP]
> **Back up your save** now and then: copy the `save` folder somewhere safe while the game is closed. To restore it, close the game and put the copy in place of the `save` folder.

## Problem reports

When the game crashes, or freezes with no new picture for five seconds, WindowSmash writes a report into the **`reports\`** folder:

| File | Written when |
| :--- | :--- |
| `crash_<time>.txt` | The game crashed. |
| `freeze_<time>.txt` | No new picture for 5 seconds (again at 30 and 120 seconds). |
| `stopped_<time>.txt` | The game stopped with an error message. |
| `<…>.dmp` | Next to a crash or freeze report: a minidump of the game's memory. |

When you report a problem, attach the `.txt` report and the end of `build\play.log`. Read them first: they can contain folder names, player names and internet addresses. **Share a `.dmp` file only privately**, with someone who asked for it, because it contains part of the game's memory. [Getting help](../README.md#getting-help) lists what else to include.

If the game doesn't start at all, check, in this order: the game data path ([step 10](BUILDING.md#step-10-connect-your-game-data)), that the game is version 13.0.5 (a game built from another version says so when it starts: [what to do](UPDATES.md#a-folder-built-from-another-version)), the six font files, that your processor has SSE4.2 at the least (a game built for AVX2 says so when it starts on a processor without it, and what to do: [the instruction set](BUILDING.md#the-instruction-set)), and your graphics driver.

WindowSmash is experimental. That it starts doesn't mean every fighter, stage, mode or online match works.
