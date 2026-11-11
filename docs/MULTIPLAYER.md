# Multiplayer

[← README](../README.md) · [Setup guide](BUILDING.md) · [Playing](PLAYING.md) · [Linux](LINUX.md) · [Technical guide](TECHNICAL.md)

WindowSmash plays the game's **Local Wireless** mode over the internet. To the game, the other players look like Switches nearby. The game's own **Online** mode works too, for Quickplay against WindowSmash's own game server ([Online mode](#online-mode-quickplay)). Nintendo's online service isn't used.

> [!NOTE]
> **Tested so far:** connecting and finding rooms, including the online lobby with several games on one PC, through the public lobby, and between a Windows and a Linux PC. Full matches between two PCs, every DLC fighter online, and every kind of home network haven't been fully verified yet. Online mode is newer still: see [what works](#online-mode-quickplay).

## Contents

- [Choose how to play](#choose-how-to-play)
- [Before you play](#before-you-play)
- [Online lobby](#online-lobby)
- [Online mode (Quickplay)](#online-mode-quickplay)
- [Direct connections and your internet address](#direct-connections-and-your-internet-address)
- [Private room](#private-room)
- [If a room doesn't appear](#if-a-room-doesnt-appear)
- [Run your own lobby](#run-your-own-lobby)
- [Rollback netplay through the lobby or a private room](#rollback-netplay-through-the-lobby-or-a-private-room)
- [Logs and privacy](#logs-and-privacy)
- [Reference: environment variables](#reference-environment-variables)

## Choose how to play

| Way | Best for | Router setup | Start with |
| :--- | :--- | :--- | :--- |
| [**Online lobby**](#online-lobby) | Playing with anyone | None | `Play SSBU.bat` |
| [**Lobby code**](#friends-only-a-lobby-code) | Friends only, through the lobby | None | `Play SSBU.bat` |
| [**Online mode**](#online-mode-quickplay) | Quickplay with anyone searching (new) | None | `Play SSBU.bat` |
| [**Private room**](#private-room) | A group with one host | The host may need to open a port | `launchers\Host SSBU Netplay.bat` and `launchers\Join SSBU Netplay.bat` |
| [**Your own lobby**](#run-your-own-lobby) | Communities with a server | The server's port | `Play SSBU.bat` |

The online lobby is the easiest: nobody hosts anything outside the game, and nobody touches their router.

On Linux ([Linux guide](LINUX.md)), `run.sh`, `run-offline.sh`, `host-netplay.sh` and `join-netplay.sh` do what `Play SSBU.bat`, `launchers\Play SSBU Offline.bat`, `launchers\Host SSBU Netplay.bat` and `launchers\Join SSBU Netplay.bat` do. Windows and Linux players play together.

## Before you play

Whichever way you choose:

1. **Everyone uses the same WindowSmash, with the game at version 13.0.5.** WindowSmash builds no other version, so every player can play with every other. The online lobby turns away a game an older WindowSmash built from another version: its overlay says `Netplay: WindowSmash needs game 13.0.5: run the installer again` ([what to do](UPDATES.md#a-folder-built-from-another-version)).
2. **Every PC is set up with its own game data**, following the [setup guide](BUILDING.md). Match the DLC between players too.
3. **Every PC keeps its own save and its own `netplay_id.txt`.** Never copy `netplay_id.txt` to another PC: two PCs with the same one look like one player.
4. **Check the game works offline first** with `launchers\Play SSBU Offline.bat`, then close it.

Each WindowSmash window is one player. A private room holds up to eight members, and the game's own room rules decide how many can play a match.

## Online lobby

This is what `Play SSBU.bat` does by default. Every Local Wireless room that anyone creates shows up for every player in the lobby.

1. Double-click **`Play SSBU.bat`**. The first time, the game picks the name other players see, such as `Player1234`, and shows it at the bottom of the screen. To change it, press <kbd>Esc</kbd> in the game, go to **Online**, choose **Your name**, then **Game → Restart the game** ([settings](PLAYING.md#settings)).
2. If Windows Security asks about `ssbu.exe`, click **Allow**. No port forwarding is needed: your game only sends to the lobby.
3. In the game's main menu, choose **Local Wireless**.
   - **To host:** choose **Create a Room**. When a challenger appears, accept them.
   - **To join:** open **Rooms List** and pick a room. Open rooms come first, fuller ones before emptier ones. The list shows up to 24 rooms (the game on its own stops at 10); scroll down for the rest. With more rooms than that in the lobby, any of them can be reached from the settings' **Lobby** page (version 3, below): pick a room there and it comes first in your Rooms List, or page the Rooms List 24 rooms at a time.
4. Press <kbd>F1</kbd> to see the connection, for example `Online: 37 players, 4 rooms - you are Alpha, ping 48 ms`. A second line lists the other players in your room and how each one is reached ([see below](#direct-connections-and-your-internet-address)).

`online_server.txt`, in the WindowSmash folder, names the lobby. Its first line that doesn't start with `#` is the lobby's address. The default, `auto`, joins the public lobby this PC reaches fastest: Europe (`opensmash-lobby.duckdns.org:39421`) or North America (`smash.recomps.wtf:39421`), timed by a connect to each at start; `eu` or `na` pins one. The two lobbies don't share rooms or Quickplay yet, but a room code reaches its host from either. The North American address alone, the old default, now means `auto` too. Port 39421 is the lobby with the game server for [Online mode](#online-mode-quickplay); port 39420 is the older lobby without it, which older WindowSmash versions use. Rooms on one don't show up on the other, so everyone in a group should use the same one.

### Friends only: a lobby code

A lobby code hides your rooms from everyone who doesn't use the same code.

1. Press <kbd>Esc</kbd> in the game and go to **Online**.
2. Set **Play online in** to **private lobby**, choose **Lobby code**, and type your code.
3. Choose **Restart now**. Everyone in your group does the same, with the same code.

The window `Play SSBU.bat` opens before the game does the same: set **Lobby** to **Private lobby (a code)** and type your code.

Capital letters count: `OurCode` and `ourcode` are two different lobbies.

Or, with the game closed, add a space and your code after the address in `online_server.txt`, for example:

```text
smash.recomps.wtf:39421 ourcode
```

The overlay shows the code after `Online`. A lobby code is not a password: anyone who knows it can join.

### Room browser and matchmaking (being tried out)

Netplay version 3 adds two things to the online lobby, on the **Lobby** page of the settings: a list of every room of your lobby code, the ones you can't join too, and matchmaking, which pairs you with another player who is searching. Version 2 is still the default.

1. Press <kbd>Esc</kbd> in the game and go to **Online**.
2. Set **Netplay version** to **3**, then choose **Restart now**.
3. Open the settings again and go to the **Lobby** page (the number key <kbd>5</kbd>).

- **Rooms** lists each room's host, players, and whether it is open. A room you can't join says why: another game version, another WindowSmash version (and whose is older), or other mods. With the cursor on a room, the help under the list shows its game build, WindowSmash (simulation) version and netplay version next to yours, and its mods, each marked **you don't have it** or **yours differs**; for a room you can't join, the mods only you have too (turn them off on the **Mods** page to join). ARCropolis' code patches count as a mod here. The list is asked for each time the page opens; select **Rooms** to ask again. It shows 30 rooms at a time: **More rooms** and **Earlier rooms** move through the rest, however many there are. To join a room, select it (it says **picked: first in your Rooms List**), close the settings and pick it in **Local Wireless → Rooms List**, where it now comes first. With more than 24 rooms you can join, **Game's Rooms List shows** pages that list too, 24 rooms at a time.
- **Find a match** searches for another player with the same game, WindowSmash, mods and lobby code. You can close the settings while it searches: a message says when a match is found and who hosts. The host creates a room (**Local Wireless → Create a Room**), and the other player finds it first in their **Rooms List**. **Stop searching** or **Leave the match** ends it.

With version 3 set, a lobby or private room of version 2 still works: the game joins it in version 2, and the Lobby page says why it has no rooms to show. Hosting a private room always uses version 2. `SSBU_NET_PROTOCOL=3` ([below](#reference-environment-variables)) or `3` in `online_protocol.txt` does the same as the setting.

### Playing offline

- `launchers\Play SSBU Offline.bat` never goes online.
- To make `Play SSBU.bat` start offline too, set **Play online in** to **offline** in the settings (**Online** page), or write `off` in `online_server.txt` in place of the address.
- If the lobby doesn't answer for five seconds, the game plays offline by itself and keeps trying in the background.

## Online mode (Quickplay)

The game's own **Online** menu works with WindowSmash's game server, which runs inside the online lobby. **Quickplay** matches you with other WindowSmash players who are searching at the same time.

> [!WARNING]
> Online mode is new. Tested on one PC (two games, a lobby of their own): logging in, the Online menus, Quickplay's search, two players matched, the whole match with the game's own netcode and with [rollback](#rollback-in-online-mode), the results with GSP, and both games back in Quickplay for the next search. Not tested yet: two PCs, more than two players. The Linux version has Online mode built in, but it hasn't been tried there.

1. Start the game with **`Play SSBU.bat`**. `online_server.txt` must name a lobby with the game server: `smash.recomps.wtf:39421`, the default, has one.
2. In the main menu, choose **Online**, then **Smash**, then **Quickplay**.
3. Choose your rules and fighter, then **Ready to fight**. The game waits in its practice room until it finds another player.

- **Who you meet:** players with the same lobby code (or none) who are searching too: the closest rating first, and, when nobody fits your search (an Elite Smash player's search, or a rating far from everyone else's), whoever is searching rather than nobody.
- **Your account:** the game server knows you by your `netplay_id.txt` and keeps your player name and ratings. Keep that file to keep your account, and don't give it to anyone: a copy of it plays as you.
- **Your connection:** the game reaches the game server through the lobby, over TCP and UDP on the lobby's port. Nobody needs to open a port. Once players are matched, the match's traffic goes through the lobby: [direct connections](#direct-connections-and-your-internet-address) are only for Local Wireless rooms so far.

| What you see | What to check |
| :--- | :--- |
| An error as soon as you choose **Online** | Start the game with `Play SSBU.bat`, not `launchers\Play SSBU Offline.bat`, and check that the overlay (<kbd>F1</kbd>) says `Online`. |
| An error code starting with **2306** | The game can't reach the game server. The lobby in `online_server.txt` has none (port 39420, or a lobby of your own run with `ssbu_relay.py`), or something blocks TCP to it. |
| Quickplay keeps waiting | Nobody with your lobby code is searching right now. Find players on the [OpenSmash-Ultimate Discord](https://discord.gg/pkgyYGf3YB). |
| "The connection was interrupted due to excessive inactivity" or "... self-destructs" | The game's own Online rules: a player who stands still or falls off too often is sent back to the menu. |

### Rollback in Online mode

> [!NOTE]
> Off by default (from v0.3.0-alpha.4): online matches are played with the game's own netcode, the stable one, until rollback has passed tests between two PCs. To try rollback, set **Online > Rollback netcode** to **on** in the settings (Esc; it writes `on` in `online_rollback.txt`, and counts from the next start), or start the game with `SSBU_RB=1` and `SSBU_RB_NET=online`. A match uses it only when both players have it on. A game started offline has no rollback at all and costs nothing for it.

With it, a Quickplay match is played with rollback netplay ([technical guide](TECHNICAL.md#rollback-netplay-ssbu_rb)) instead of the game's own delay-based netcode: your own inputs answer at once (after a small input delay), the other player's are guessed until they arrive, and the game quietly re-runs the few frames a wrong guess touched. Everything around the fight stays the game's own: matchmaking, the character screen, the results with GSP, the next search.

- **Both players need it.** As the match is set up, the two games agree on rollback within 15 seconds; when the other game has it off (or is an older build), the match is played with the game's own netcode, as without it. A build from before rollback was on by default has it off unless its player turned it on with `SSBU_RB=1` and `SSBU_RB_NET=online`.
- **Two players.** A Quickplay match of three or four players is played with the game's own netcode.
- **Input delay:** chosen from the round trip the two games measure as the match is set up (the same on both). `SSBU_RB_NET=online,<frames>` sets it by hand.
- **Desyncs and lost players:** both games compare a hash of the fight every few frames. If the other game stops answering for 8 seconds, or the two go out of sync, a box says so and the game writes `reports\rollback_<date>_<time>.txt` with its last rollback lines: send it with `play.log`.

How it behaves at a given round trip (two games on one PC, normal speed, the round trip simulated with `SSBU_NET_SIM`; a frame is 16.7 ms):

| Round trip | Input delay | Rollbacks in a 3-minute match (each game) | Frames re-run in one | Time to re-run them (mean, worst) | Waits for the other | Fight hashes at the end |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| ~1 ms (same PC) | 1 (auto) | 1-6 | 2 | 7-10 ms | none | identical |
| ~65 ms | 1 (auto) | 800-1200 | mostly 2-3 | 11-12 ms (27 ms) | none | identical |
| ~110 ms | 1 (forced) | 770-1300 | mostly 3-5 | 17-20 ms (40 ms and more) | none | identical |
| ~115 ms | 2 (auto) | 790-1280 | mostly 2-3 | 13-15 ms (41 ms) | none | identical |

A re-run under 16.7 ms is not seen; one over it shows as a frame late. That is why the delay is the one-way trip in frames less one (1 only under 15 ms round trip, a home network; otherwise at least 2, up to ~115 ms 2, up to ~150 ms 3, at most 4): with less, the re-runs get longer than a frame; with more, every press waits longer. The floor of 2 comes from a live two-PC match at a 40 ms round trip, with people playing: delay 1 made 1263 rollbacks in one fight, ran at about 50 frames a second and desynced; delay 2 made 15 rollbacks in two fights, 60 frames a second, in sync. The table above is from before it. The bots of these tests press something every few frames, far more changes than a person makes, so a real match rolls back less. "Fight hashes at the end": every frame's hash, as each game kept it once both players' inputs were known, compared over the whole match.

Two games on one PC against a lobby of your own, for a test (each game in a folder of its own, with its own save and `netplay_id.txt`):

```powershell
python tools/ssbu_online_lobby.py --bind 127.0.0.1 --port 39421 --state online-state --verbose
# both games: started with --online, online_server.txt (or SSBU_NET_RELAY) naming 127.0.0.1:39421: rollback is on by default
# tests only: SSBU_RB_NET_BOT=calm (a bot plays each game's player and stays on the stage; without it, the controllers play), SSBU_RB_NET_BOT_CSTICK=<player 1-8> (that player's bot jumps and pushes the C-stick in the air, its magnitude changing every frame: #181's case), SSBU_NET_SIM=<ms>[,<loss %>[,<jitter ms>]] (each game holds what it receives that long)
```

## Direct connections and your internet address

Online, the games in a room send their traffic **straight to each other** whenever the players' routers allow it. The lobby introduces them, and the games open a path through their routers themselves (UDP hole punching), so nobody forwards a port for this either. Where that doesn't work, the traffic keeps going through the lobby. A direct path that goes quiet falls back to the lobby within about a second; a relayed one keeps being tried during the match and switches to direct as soon as it opens.

Through the lobby, the delay between two players is roughly both players' pings to the lobby added together: the traffic goes from one player to the lobby and on to the other. The public lobbies run in Europe (Milan), so two players in North America who end up relayed cross the Atlantic twice, about 100 ms or more. Directly, it's just the delay between the two PCs.

**Routers that change ports.** Some routers give every destination a port of their own (a *symmetric NAT*: some providers' routers, many mobile networks, providers that share one address between many customers). The lobby then sees one port and the other player's game another. The game works around it on its own:

- it answers a player at the address that player's hellos really come from, which joins a router that changes ports with most others;
- it asks your router to forward its port (UPnP, NAT-PMP or PCP, as a console does), and tells the other players that port;
- where a router hands out ports in turn, the lobby sees two of them (its second port, the *NAT port*) and the other game tries the next few.

When none of these works (for example two routers that change ports at random), the room goes through the lobby, and the game says so in its log (`[net] no direct path to ...`) and on <kbd>F1</kbd>: `relayed (your router changes ports)` means turning on **UPnP** or **NAT-PMP** in your router's settings gives direct paths; `relayed (their router changes ports)` means the other player's router needs it. At the end the log counts the session: `direct paths this session: 3 of 4 players introduced; relayed 1 (...)`.

The overlay's second line shows each player in your room, for example `Room: Bravo direct 12 ms, Kim relayed`:

| Shown | Meaning |
| :--- | :--- |
| `direct 12 ms` | Straight to that player, 12 ms there and back. |
| `relayed` | Through the lobby. |
| `relayed (trying direct)` | Still opening a direct path. |
| `relayed (your router changes ports)` | Your router gives every destination its own port: turn on UPnP or NAT-PMP in it. |
| `relayed (their router changes ports)` | The same for that player's router. |
| `relayed (direct off)` | That player turned direct connections off. |
| `relayed (direct off here)` | You turned them off. |
| `relayed (older game)` | That player's WindowSmash is too old for direct connections. |

Direct connections need a lobby running this version of `tools/ssbu_relay.py`. With an older lobby, or a player with an older WindowSmash, play goes through the lobby as before.

> [!NOTE]
> **Your internet address.** The players in a room with you learn your internet (IP) address, as they would when playing online on a console. The lobby introduces only players whose games are already playing together in a room, and introduces each player in at most 20 rooms an hour.
>
> To keep your address to yourself, turn **Direct connections** off in the settings (**Online** page), then restart the game. Or create a file named `online_direct.txt` next to `Play SSBU.bat` (on Linux, next to `run.sh`) containing the word `off`. All your traffic then goes through the lobby, which gives your address to nobody.

Private rooms always go through the host.

## Private room

One player hosts a room on their own PC; the others join it with the host's address. Agree on a room name first, for example `smash`.

### The host

1. Double-click **`launchers\Host SSBU Netplay.bat`**.
2. It asks three questions:
   - **Your player name:** type your name and press <kbd>Enter</kbd>.
   - **Room name:** type the name you agreed on and press <kbd>Enter</kbd>.
   - **UDP port:** press <kbd>Enter</kbd> to keep **39420**.
3. If Windows Security asks about `ssbu.exe`, click **Allow**. If you set up your firewall by hand, it must let in **UDP** on that port.
4. The launcher asks your router to open the port (UPnP). The address to give your friends, for example `203.0.113.7:39420`, appears in the launcher's window or in `netplay_address.txt`, and in the game's overlay (<kbd>F2</kbd>). Send it to the others. `netplay_address.txt` also has the same address as a **room code**, ten letters and digits such as `5ZQ9P-RK1Y8`, and one for friends on your own network: a friend can type the code wherever an address goes (case, dashes and spaces don't matter).
5. In the game's main menu, choose **Local Wireless** (the icon of a console with waves, in the right-hand column), then **Create a Room**. When a friend's challenge appears, accept it.

The address above is only an example. Use the one your own launcher shows, or a VPN or home-network address ([which address](#which-address-to-give)).

### Joining players

1. Double-click **`launchers\Join SSBU Netplay.bat`**.
2. It asks three questions:
   - **Host's address:** type the address the host sent you, including the `:` and port, and press <kbd>Enter</kbd>.
   - **Your player name:** type your name and press <kbd>Enter</kbd>.
   - **Room name:** type **exactly** the host's room name and press <kbd>Enter</kbd>.
3. In the game, press <kbd>F2</kbd> and check that the relay is reached and the player count looks right.
4. Choose **Local Wireless**, pick the host's room from **Rooms List**, and wait for the host to accept.
5. If the game asks for a Smash Tag, make your own.

Joining players don't need to change anything on their routers.

The host's game only lets in players whose game matches its own: the same game build, the same WindowSmash, and the same mods (ARCropolis' code patches included). A player who doesn't match is told why, in the overlay (<kbd>F2</kbd>), on the settings' **Online → Now** row and in the log: which build each side has, or which mods only one side has or has in another copy, or that the code patches are on for one and off for the other. A player on an older WindowSmash, which doesn't say its mods, is let in as before.

### On the same network: no address needed

Players on the host's own network (the same home router, or one PC running two games) can skip the address and the room code. The host's game announces its room on the local network every second while it hosts, and a game set to look for it joins it by itself.

1. The host starts hosting as above.
2. Each other player sets the settings' **Online → Play online in** to **this network (LAN)** and restarts the game (or writes `lan` in `online_server.txt`, or starts the game with `SSBU_NET_RELAY=lan`).
3. The game listens for about a second and a half, then joins the first room it heard from a game of the same version, in that host's room. The overlay (<kbd>F2</kbd>) and **Online → Now** say which, and the **Lobby** page shows **Rooms on this network** with the count. If no host has started yet, it says `no room on this network yet (still looking)` and keeps listening: it joins as soon as a host starts, and moves to the next host if its host leaves.

To join only one room among several hosts, give the room name too: `lan ourroom` in `online_server.txt`, or `SSBU_NET_ROOM=ourroom`. A room of another game build or WindowSmash version is heard but not joined (the Lobby page counts it apart). The host's game still checks the mods as for any joining player.

The announcement is a small UDP packet to the multicast group `239.255.13.13` and to the broadcast address, port **39501**, that routers don't pass on. The host's firewall must let in UDP on its relay port (39420), as for any player on its network; the looking game listens on port 39501, so Windows Security may ask about `ssbu.exe` the first time. The host doesn't announce its room while **Direct connections** is off (the player keeps their address to themselves) or with `SSBU_NET_LAN=0`: players on its network then use the room code for this network in `netplay_address.txt`, or its address.

**Two games on one PC**, to try it: start the host with `SSBU_NET_HOST=1`, and the second game with `SSBU_NET_RELAY=lan` (each in a folder and with a `netplay_id.txt` of its own). The second game finds the first one's room through the PC's own copy of the multicast packet.

### Which address to give

| Where the players are | Address the others type | What the host sets up |
| :--- | :--- | :--- |
| Same home network | Nothing: Play online in > this network ([above](#on-the-same-network-no-address-needed)), or the host PC's local address and port | Let incoming UDP through the host's firewall. |
| Different homes, UPnP works | The public address the launcher shows | Nothing more: the launcher opened the port. |
| Different homes, port forwarded by hand | The host's public address and port | Forward **UDP 39420** on the router to the host PC; allow it in the firewall. |
| Everyone on one VPN or virtual LAN | The host's VPN address and port | Everyone joins the same virtual network and allows traffic between its members. |
| A relay on a server | The server's address and port | Everyone, the host included, uses `launchers\Join SSBU Netplay.bat`. |

If your internet provider uses carrier-grade NAT, or your router sits behind another router, forwarding a port at home may not be enough. A virtual LAN, a relay on a server, or the [online lobby](#online-lobby) is usually easier.

## If a room doesn't appear

| What you see | What to check |
| :--- | :--- |
| The overlay stays on `connecting` | The host's address and port, that the host's game is still running, the host's firewall rule for incoming UDP, the router's port mapping, and whether the relay can be reached. |
| `Netplay: no room on this network yet (still looking)` | Set to this network, the game hasn't heard a host of its version yet. Check that the host's game is hosting, that both PCs are on the same network (not a guest Wi-Fi that keeps devices apart), and that the host's **Direct connections** is on. With `SSBU_NET_LOG=1` the log names every room heard. |
| `no answer from … - playing offline` | The lobby in `online_server.txt` hasn't answered for five seconds. Check the address with `python tools/ssbu_relay.py --probe <address>`. |
| `Netplay: WindowSmash needs game 13.0.5: run the installer again` | Your game was built from another version of the game, by an older WindowSmash. Put the game's 13.0.5 update in your game folder and run `Install WindowSmash.bat` again: it sets the folder up again as 13.0.5, keeping your save and settings ([how](UPDATES.md#a-folder-built-from-another-version)). |
| `Netplay: the host runs other mods` (or `another game version`, `another WindowSmash version`) | The lines under it say what differs: the mods only the room has, the ones you have in another copy, the ones only you have, or the code patches on one side only. Turn mods on or off on the settings' **Mods** page (or in the launcher) to match, and restart. |
| Online, a room is missing from the list | Both players need the same lobby code (or none), and both overlays must say `Online`. The list refreshes every few seconds. |
| Both are connected, but in different groups | Private-room names must match exactly. |
| The relay count is right, but no Local Wireless room | The host must create a room in the game: being connected doesn't create one. Wait for the list to refresh. |
| Two people show up as one player | Their installs share an identity. Close the game on one PC and **rename** its `netplay_id.txt`, so a new one is made. Keep both saves. |
| *Communication error* after joining | Packet loss, whether the relay can be reached, matching DLC, and whether the host or relay closed or changed networks. |
| Works at home, not over the internet | The host's firewall, UDP port forwarding, double NAT or carrier-grade NAT. Or use a virtual LAN, a relay on a server, or the online lobby. |
| Online, a player is always `relayed` | Their router or yours doesn't allow direct paths (symmetric NAT, common on mobile networks), a firewall drops the game's packets, or the lobby is older than direct connections. Play still works through the lobby. If Windows Firewall was told to block `ssbu.exe`, allow it in **Windows Security → Firewall & network protection → Allow an app through firewall**. |
| It works, but feels delayed | Traffic through a relay or the lobby adds both players' pings to it. Pick a host or lobby close to the players, and check the overlay for players who aren't `direct`. |

In a private room, the host has no relay hop to itself; each joining player's traffic goes through the host's relay, including traffic between two joining players. Local Wireless was designed for players in the same room, so internet delay can change how the game behaves.

## Run your own lobby

`tools/ssbu_relay.py` is the online lobby. It needs **Python 3.8 or newer**, a UDP port others can reach, and no game files:

```powershell
python tools/ssbu_relay.py --port 39420 --verbose
```

1. Let incoming UDP 39420 through that machine's firewall, and any network firewall in front of it, and UDP 39430 too: the lobby's NAT port (`--nat-port`, the lobby's port + 10 unless given; `0` for none), where games compare their routers' ports for direct paths. ssbu-recomp's games on `--su-port` 39500 use 39510 (`--su-nat-port`).
2. Every player puts the lobby's address in `online_server.txt` and starts the game with `Play SSBU.bat`.
3. One player creates a Local Wireless room; the others pick it.

`launchers\Join SSBU Netplay.bat` with the lobby's address works too; the room name then acts as the lobby code.

| Task | How |
| :--- | :--- |
| Check that a lobby answers | `python tools/ssbu_relay.py --probe <address>` |
| Install it on a Linux server | `tools/deploy/deploy_lobby.sh <ssh host> install` sets it up as a sandboxed systemd service, `ssbu-lobby`. |
| Turn addresses away | `--banlist FILE` refuses the addresses or networks listed in that file, and re-reads it when it changes. |
| Watch it | It logs a line of statistics every minute. |

To offer [Online mode](#online-mode-quickplay) too, run `tools/ssbu_online_lobby.py` in place of `ssbu_relay.py`. It takes the same options, plus `--state <folder>` for the players' accounts and ratings, and it needs **TCP as well as UDP** on its port:

```powershell
python tools/ssbu_online_lobby.py --port 39421 --state online-state --verbose
```

`tools/deploy/deploy_online_lobby.sh <ssh host> install` sets it up on a Linux server as the service `ssbu-online`, next to `ssbu-lobby`. Its game server lets each player act only on their own account and on matches they're in, and limits how much each connection can ask of it.

The lobby checks where messages come from, limits how fast each player can send, and carries each room's traffic only between that room's players. A game accepts direct packets only from addresses the lobby gave it, carrying that pair's secret, and only from players in its own room. Even so, it's a transport for players you trust: lobby codes and room names aren't passwords, and the traffic isn't encrypted or authenticated.

## Rollback in Local Wireless rooms

Started with `Play SSBU.bat` (or the launcher), a Local Wireless room's 1-on-1 match is played with rollback too when both players have turned it on: the room is made and joined as usual (in the lobby, or in a private room), and as the match is set up the two games agree on rollback within 15 seconds, exactly as a [Quickplay match](#rollback-in-online-mode) does. The character screen, the stage and the results stay the room's own. When the other game has rollback off (or is an older build), or the room has three or four players, the match is played with the game's own netcode. The same switch turns it on: **Online > Rollback netcode** in the settings (off by default).

## Rollback netplay through the lobby or a private room

Rollback netplay (`SSBU_RB=1`, being tried out: [technical guide](TECHNICAL.md#rollback-netplay-ssbu_rb)) can carry its messages through the same connection as Local Wireless instead of its own UDP port: set netplay up as usual (the lobby, or a private room) and add `SSBU_RB_NET=relay`. Nobody opens a port for it; a private room's host needs only the relay port it already has. Games in one Local Wireless room of the lobby send straight to each other when their routers allow it, and through the lobby otherwise.

- **Who is player 1:** with `SSBU_RB_NET=relay` (or `relay,auto`), the players are numbered by their address in the room, lowest first, once the room has `SSBU_RB_NET_PLAYERS` players (2 by default; set the same on every PC). In a private room the host's game is player 1.
- **A lobby code without a room:** players of one lobby code (not `smash`) who aren't in a Local Wireless room together give their numbers instead: `relay,0` for the host, `relay,1` (or `relay,-1`: the host gives one) for the others, with `SSBU_NET_PROTOCOL=3`.

Two games on one PC through your own lobby, for a test:

```powershell
python tools/ssbu_relay.py --bind 127.0.0.1
# host:   SSBU_RB=1 SSBU_NET_RELAY=127.0.0.1:39420 SSBU_NET_PROTOCOL=3 SSBU_NET_ROOM=rbtest SSBU_RB_NET=relay,0
# joiner: SSBU_RB=1 SSBU_NET_RELAY=127.0.0.1:39420 SSBU_NET_PROTOCOL=3 SSBU_NET_ROOM=rbtest SSBU_RB_NET=relay,1
```

## Logs and privacy

The launchers write `build\play.log`. For more network detail, set `SSBU_NET_LOG=1` before starting the game (`2` logs every call). Logs and the overlay can show player names, folder names and internet addresses, so read any excerpt before you share it. With Online mode, the lobby's game server also keeps your player name and ratings.

The game server's log (`--nex-log`: `0` quiet, `1` each call, `2` every packet, `3` also a copy of each call's parameters in `<state>/requests`, never a login's; `ssbu-online` runs at `1`) names players by name, player ID and account tag: the first 8 hex digits of a SHA-256 of their account id. At no level does it hold the account id itself or the login token. The account id is what logs a player in, so anyone who has it can play as them. The accounts file in the `--state` folder does hold every player's account id, so keep that folder private. Game server logs from before this change hold each player's account id in full: delete them.

## Reference: environment variables

| Variable | Meaning |
| :--- | :--- |
| `SSBU_NET_HOST=1` | Start a relay inside the game and join it (hosting a private room). |
| `SSBU_NET_RELAY=address[:port]` | Join the online lobby or the relay at this address. `lan`: join a private room hosted on this network, found by itself ([how](#on-the-same-network-no-address-needed)); `online_server.txt` takes `lan` too. |
| `SSBU_NET_ROOM` | The lobby code online, or the room name in a private room. Default `smash`, the public lobby. |
| `SSBU_NET_NAME` | The player name others see. |
| `SSBU_NET_PORT` | The relay's port. Default `39420`. |
| `SSBU_NET_UPNP=0` | Don't ask the router to open the port. |
| `SSBU_ONLINE_STUN=<host:port>` or `off` | When the router opens no port, the hosting game asks this STUN server (default `stun.l.google.com:19302`) how its relay looks from outside, and gives a room code for that address: it works with routers that keep the mapping for everyone. `off`: no STUN. |
| `SSBU_NET_BIND` | Limit which address the built-in relay listens on; used by local tests. A relay on `127.0.0.1` announces its room to this PC's loopback only. |
| `SSBU_NET_LAN=0` | Hosting, don't announce the room on the local network. |
| `SSBU_NET_LAN_PORT`, `SSBU_NET_LAN_TEST=1` | Local tests: the announcements' port (default `39501`), and announcements to `127.0.0.1` only, the looking game listening there. |
| `SSBU_NET_DIRECT=0` | Online, send all traffic through the lobby. `Play SSBU.bat` sets it when `online_direct.txt` says `off`. |
| `SSBU_NET_PROTOCOL=3` | Being tried out: join the lobby in netplay version 3, which has the [room browser and matchmaking](#room-browser-and-matchmaking-being-tried-out). A lobby of version 2 says no, and the game joins it in version 2. `2`, the default, is version 2 only. Without it, `online_protocol.txt` decides (`3` or `2`; the settings' **Online → Netplay version** writes it). Hosting a private room is always version 2. |
| `SSBU_NET_UPNP=0` | Leave the router alone: no UPnP for a hosted room, and online no port mapping (UPnP, NAT-PMP, PCP) of the game's port. |
| `SSBU_NET_TEST_NAT=1`..`4`, `SSBU_NET_TEST_BLOCK=<seconds>` | Local tests: a router simulated in the game, `1` one that keeps the port and lets in only what comes from where it sent, `2` one that changes ports for every destination (at random), `3` the same in turn, `4` one that lets in anything from an address it sent to; or cut direct paths that many seconds after the start. `SSBU_NET_TEST_UPNP=1`: that router forwards the game's port. `SSBU_NET_TEST_LEARN=0`: no address learnt from a player's hellos. `SSBU_NET_TEST_GATEWAY=<ip:port>`: the PCP / NAT-PMP gateway to ask (no UPnP). |
| `SSBU_NET_TEST_SAVE_ROOM=<file>`, `SSBU_NET_TEST_ROOMS=<file>[,<n>]` | Local tests of the Rooms List: a host writes its room's network to the file once a second; a game given that file also finds n rooms made from it at every scan (default 15, up to 24; `clone[,<n>]` copies the first room really found). They can't be joined. |
| `SSBU_NET_LOG=1` / `2` | Network logging: a summary, or more detail. |
| `SSBU_RB_NET=relay[,<player>[,<delay>,<frames>]]` | With `SSBU_RB=1`: rollback netplay's messages through this netplay connection. `<player>`: `auto` (by address in the room), `0`-`3`, or `-1` (the host gives one). See [above](#rollback-netplay-through-the-lobby-or-a-private-room). |
| `SSBU_RB_NET=online[,<delay>]` | The game's own matches with rollback: Quickplay ([Rollback in Online mode](#rollback-in-online-mode)) and Local Wireless rooms ([Rollback in Local Wireless rooms](#rollback-in-local-wireless-rooms)); the players and their numbers come from each match. The default for a game started with `--online`. |
| `SSBU_RB=0` | No rollback: the game's own netcode for every match, also when started with `--online` (as **Online > Rollback netcode** off). `SSBU_RB_NET=off` keeps the savestates but no rollback netplay. |
| `SSBU_ONLINE_COUNTRY` | The country Online mode reports, as two letters such as `GB`. Default `US`. |
| `SSBU_ONLINE_SERVER` | `off` turns Online mode off. Otherwise the game server's address inside the lobby, `10.13.0.1:60000` by default; only for tests. |

Without a host or relay setting, the game stays offline. `launchers\Play SSBU Offline.bat` clears both; `Play SSBU.bat` takes the relay from `online_server.txt`; the host and join launchers set their own.
