# Test runs

[← Technical guide](TECHNICAL.md) · [Contributing](../CONTRIBUTING.md)

These switches make a scripted run of the game quicker and less dependent on the PC it runs on, and they are meant to make a fight repeat exactly. They are test aids, not settings for playing. Each one is off unless its environment variable is set, and with it unset the game runs exactly as before. They were ported from ssbu-recomp (`ssbu-recomp/tests/DEBUG_MODE.md` and `ssbu-recomp/fighter_sdk/fighter_host.c`). WindowSmash builds game version 13.0.5 only.

**Status: not yet run with the game.** The sequence parser, the step machine and the arithmetic of the speed and the movie count were tested offline on made-up input (`tests/test_test_runs.py`). So were the fight switches' logic and their hooks, on made-up guest memory (`tests/test_fight_test.py`), and the hash log comparison (`tests/test_fight_hash_diff.py`). The runtime files compile against the macOS shims. None of these switches has run with the game in WindowSmash, on any platform. Their Windows and Linux builds were not compiled either. This guide says what each switch is built to do. Where that rests on something ssbu-recomp measured, it says so. Each section ends with what a run still has to check.

| Variable | What it does |
| :--- | :--- |
| `SSBU_SPEED=<n>` | Fast-forward: one display refresh per frame shown, at n × 60 a second; `0` is uncapped. `1` or unset is normal pacing. |
| `SSBU_SEQ=<steps>` | Player 1's input as a sequence counted in the game's frames, not in seconds. Empty, or with no steps, it is the same as unset. |
| `SSBU_SEQ_LOG=<n>` | With `SSBU_SEQ`: logs the draw calls of the last frame shown every n polls, to find the ranges for `@draws`. |
| `SSBU_SEQ_KEYBOARD=1` | With `SSBU_SEQ`: the keyboard still plays. It is ignored otherwise. |
| `SSBU_SKIP_MOVIES=<n>` | Every movie's stream ends after n samples (`1` means 12), and the game ends the movie as it ends any movie. |
| `SSBU_FIGHT_HASH=<file>` | One line per fighter per fight frame, with a running hash, to `<file>` (`1`: to the standard output). |
| `SSBU_DETERMINISTIC=1` | Meant to make the same input give the same fight: fixed random generators, no extra logic frames, `SSBU_SEQ` counted in fight frames. `SSBU_DET_OFF=<mask>` turns parts off. |
| `SSBU_FIXED_SEED=<n>` | The clock reads and random bytes the game seeds its generators from come from n. |
| `SSBU_DEBUG_MATCH=<settings>` | The fighters, stage and rules of the match, whatever the menus chose. `SSBU_DEBUG_MATCH_LIST=<file>` takes one line per match. |
| `SSBU_DEBUG_END=<n>s` | The run ends after n seconds of fight (`<n>` without `s`: n fight frames). |

`SSBU_INPUT` (buttons on a timetable in seconds) is unchanged, and it still adds to player 1 when `SSBU_SEQ` is set.

## `SSBU_SPEED`

Normally the vsync thread gives the game 60 refreshes a second whether or not a frame is ready ([`ipc_vi.c`](../runtime/ipc_vi.c)). The game counts those refreshes and runs extra logic frames when it sees it has fallen behind, so a slow PC plays a slightly different game than a fast one.

With `n` other than 1 there is no refresh clock:

- **One refresh per frame shown.** The vsync thread waits for the frame the game queues, shows it and then gives exactly one refresh. The game should then step one frame per frame shown, however fast or slow the PC is. ssbu-recomp measured that at 4× and uncapped: 600 game steps per 600 frames shown. It also waited for the game to count each refresh, which this runtime does not do yet (see below).
- **No refresh without a frame.** While the game queues nothing (a load, a shader compile), it gets no refresh, however long that lasts. A refresh there would count as a dropped frame and run an extra logic frame. ssbu-recomp gave none either, and the game still booted to a fight at 2×, 4× and uncapped.
- **The pace is n × 60 frames a second at most** (`0`: as fast as the game draws, which can be slower than 60). After a stall such as a load, there is no burst to catch up.
- **Frames are shown at once.** They are not held to the monitor's refresh: Direct3D gets the tearing flag where the swap chain allows it, and macOS turns off the display sync.
- **The game's own waits in seconds don't speed up.** ssbu-recomp found that the movie's pre-roll and the loading threads take four times as many frames at 4×. Wait for screens with marks and `@draws`, not with counts of ticks.
- **A line a second:** `[display] speed: 240 frames shown in the last 1.00 s (240 a second)`. During a load it says 0 frames.

What it does not guarantee: the game counts refreshes on a thread of its own and compares the count at the end of each frame. If that thread runs late, one frame can still see two refreshes and run two logic frames. ssbu-recomp waited until the game had counted each refresh; this runtime does not read that count yet. The deterministic mode ([`SSBU_DETERMINISTIC`](#ssbu_deterministic)) removes those extra steps.

**Audio is not touched.** The sound device still sets the pace of the game's mixer ([`ipc_audio.c`](../runtime/ipc_audio.c)), so the sound does not speed up with the picture. WindowSmash has no switch to mute it (`SSBU_AUDIO` only picks the Windows output; ssbu-recomp's scripts used an `SSBU_AUDIO_MUTE` that WindowSmash doesn't have). Turn the volume down.

Not yet checked in a run:

- that boot reaches the menus and a fight at 4 and at 0 on Windows, macOS and Linux;
- that boot doesn't stall without refreshes. If the speed lines say 0 frames for good, the game is waiting for a refresh before it draws, which ssbu-recomp never saw: please report it;
- the rate in the speed lines;
- whether Direct3D without tearing support still runs faster than the monitor.

## `SSBU_SEQ`

ssbu-recomp found that the game reads player 1's controller once per frame (`ssbu-recomp/runtime/nnhid.c`). That has not been checked in WindowSmash yet. Each read (a *poll*) moves the sequence on by one *tick*. A hook can change that: `input_seq_set_steps` in [`input_seq.h`](../runtime/input_seq.h), which [`SSBU_DETERMINISTIC`](#ssbu_deterministic) uses to count fight frames instead.

Steps are separated by spaces or new lines, and `#` starts a comment that runs to the end of the line. Keys are the names `SSBU_INPUT` takes, joined by `+`: `A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LSTICK RSTICK`, and `SLEFT SUP SRIGHT SDOWN` for the left stick.

| Step | Meaning |
| :--- | :--- |
| `<n>` | Nothing for n ticks. |
| `<n>:<keys>[/<h>]` | Nothing for n ticks, then the keys for h ticks (8 if not given). |
| `@<mark>` | Wait until the mark has been raised (marks are listed below). |
| `@draws:<lo>-<hi>[/<n>]` | Wait until every frame shown has had lo to hi draw calls for n ticks in a row (20 if not given). A screen can be recognised by its draw count, whatever the loading time was. |
| `@until:<lo>-<hi>[/<n>]:<keys>[/<p>]` | Press the keys for 8 ticks until the draw count has been in range for n ticks in a row. The first press comes at once and the next p ticks after it began (200 if not given), each on a tick when the count is out of range. A press under way is finished first. |
| `@untilmark:<mark>:<keys>[/<p>[/<h>]]` | Press the keys for h ticks (8 if not given), at once and again every p ticks (200 if not given), until the mark has been raised. A press under way is finished first. |

ssbu-recomp found that `@until` can race a movie cut short: a press that lands just as the next screen appears can pass that screen too. With `SSBU_SKIP_MOVIES`, wait for that screen with `@draws` instead, as the example below does.

What the step machine does (tested offline on made-up sequences):

- **Exact counts.** A wait step ends on the tick its condition is met, and the next step starts on the tick after. Holds are exact: `0:A/3 2:B` is A on ticks 1-3, nothing on 4-5, then B on 6-13.
- **A sequence that doesn't parse stops the game at start**, naming the step and what is wrong with it. An empty `SSBU_SEQ`, or one with nothing but spaces and comments, is the same as unset: the log says so and player 1 plays as usual.

What the runtime does around it (not yet run with the game):

- **Player 1 is the sequence's.** Player 1's pad is not read, and player 1 is connected as a Pro Controller throughout. The keyboard plays for nobody unless `SSBU_SEQ_KEYBOARD=1`. Players 2-8 keep their pads, so unplug them for a run that has to repeat exactly.
- **Marks:** `movie` when a movie's stream is opened, `module:<name>` when the game has loaded that module (the name its `[ro] loaded` log line gives), and `fight` at the first battle frame when `SSBU_FIGHT_HASH` or `SSBU_DETERMINISTIC` is set. Up to 512 different marks are kept; past that the log says a mark was not kept, and a step waiting for it waits forever.
- **Draw counts** are the draw calls the GPU ran for each frame shown. They are not the same numbers as ssbu-recomp's, which counted at a higher level, so its ranges don't carry over: use `SSBU_SEQ_LOG` to find yours. With `SSBU_GPU_THREAD=0` they are approximate.
- **The log** has one line per press and per wait that ends: `[seq] poll 812: tick 812, step 3 (60:A): keys for 8 ticks`.

Not yet checked in a run:

- that player 1 is polled once per frame: count the polls against the frames shown;
- that the `movie` and `module:<name>` marks appear;
- that `SSBU_SEQ_LOG`'s draw counts are steady on a still screen;
- that a pad in slot 0 and the keyboard are ignored.

## `SSBU_SKIP_MOVIES`

After n samples (12 when n is 1) a movie's stream reports that it has no more data. The game should then end the movie through its normal end-of-stream path, as it did in ssbu-recomp. ssbu-recomp only tried this on the opening movie. It also found that an end before any sample is not one the game gets through, which is why the default is 12 and not 0. The count starts again when the game seeks the stream: it does that when a movie started late, and samples it read before the seek are no use to it.

It works on the movie library's `movie::Extractor` vtable in `subsdk0` ([`hle_movie_skip.c`](../runtime/hle_movie_skip.c)). If that vtable is not as expected, the log says so and movies play as usual.

**A known gap, which matters on a busy PC.** On a late start, the game can seek the stream while its player is still being prepared. By then the stream may already have ended after its 12 samples. The game's decoders have taken that end and offer no more input, so the movie waits forever and the title screen never comes. ssbu-recomp saw this in about 1 boot in 4 when two games booted at once. It fixed it by having its own decoders take inputs again after such a seek. Here the decoders are the game's own, so that fix is not possible. The log says when such a seek happens. On a machine that runs several games or jobs at once (CI), use a larger count: `SSBU_SKIP_MOVIES=120` should let the movie start playing before its stream ends. That is an inference from ssbu-recomp's analysis, not a measurement.

Not yet checked in a run:

- that the log says the `movie::Extractor` vtable was taken, not that movies are left alone;
- that the opening movie ends and the title screen comes, at 1 and at 120;
- how often the seek after an early end hangs here, on repeated boots of a busy PC.

## `SSBU_FIGHT_HASH`

One line for each fighter at each of its fight frames, written to the file, or to the standard output with `1` ([`hle_fight_test.c`](../runtime/hle_fight_test.c)). A fight frame is one call of the fighter's frame line, `lua2cpp::L2CFighterCommon::sys_line_system_control_fighter`, which the game runs for every fighter once a frame before its status scripts. The hook runs before that function, through its wrap by exported name ([`hle_each_frame.c`](../runtime/hle_each_frame.c), the frame hooks that rollback shares), so it holds wherever `lua2cpp_common` is loaded. The line is written as the frame line starts, so it shows the state the previous frame left. A made-up line:

```text
F 1200 e 0 st 1a mot 7fb997a80 mf 41200000 x c1a40000 y 00000000 dmg 41a80000 rngcalls 3124 rng 5e1d02a7 H 3c41f0a2d7b1e9c4  (-20.500 0.000 frame 10.00 21.00%)
```

- `F`: the fighter's fight frame, from 1 at its first call.
- `e`: its entry id (its player slot), the work variable `FIGHTER_INSTANCE_WORK_ID_INT_ENTRY_ID`. ssbu-recomp read that constant's value from a table its build made. Here it is asked of the game (`lib::lua_bind_get_value<int>`), and the log says whether that worked. If it didn't, `e` is -1 on every line, as in ssbu-recomp without its table.
- `st`, `mot`, `mf`: the status kind, the motion (a Hash40) and the motion's frame. `x`, `y`: the position. `dmg`: the damage percent. They come from the game's own `app::lua_bind` getters. Floats are written as their bits, so a difference in the last bit shows.
- `rngcalls`, `rng`: the 9 script random generators that `app::sv_math::rand` draws from (20 bytes each: 4 state words and a call count). The line has the sum of their call counts and their FNV-1a hash, folded to 32 bits.
- `H`: a running FNV-1a over everything logged so far, with the generators' whole hash. Two logs that agree on `H` at a line agree on every line before it.
- The part in brackets has the same numbers, for reading.

The line, its fields and their order are ssbu-recomp's, so logs of both can be compared. Lines are written out to the file every 64 fight frames, and when `SSBU_DEBUG_END` ends the run. To the standard output, each is written out as it is made, so that no other line of the log lands in the middle of one (on Windows the standard output is not line-buffered). A run stopped another way can leave its last line cut; [`fight_hash_diff.py`](#comparing-two-logs) leaves that line out. Every 300 fight frames of the first fighter, the log has a line with each generator's call count, how many fight frames each poll of player 1 saw, and how many dropped refreshes the game reported.

The first call of a fighter's frame line in the process is the **first battle frame**. The log says so, with the generators' state before it, and `SSBU_SEQ`'s mark `fight` is raised there. Up to 8 fighters are followed: the first 8 the frame line sees in the process. The log is meant for one fight per process: whether a later fight's fighters count as new ones has not been looked at.

What ssbu-recomp measured: its test for fighter mods compared two runs in the deterministic mode, an unedited copy of a fighter against the fighter it was copied from. Their hash logs were the same line for line, the random generators included (`ssbu-recomp/docs/NEW_FIGHTER.md`).

Not yet checked in a run:

- that the frame line is wrapped: hash lines appear from the first battle frame;
- that the entry id is found (the log's `FIGHTER_INSTANCE_WORK_ID_INT_ENTRY_ID is` line) and that `e` is 0 for player 1 and 1 for player 2. The mangled name of `lib::lua_bind_get_value<int>` was written by hand, not read from the game;
- that position, damage and status change as the fight goes;
- that the generators' address (`SCRIPT_RNG_ROOT` in [`game_addr.h`](../runtime/game_addr.h)) leads to 9 generators whose call counts go up in a fight;
- what the hook costs at normal speed.

## `SSBU_DETERMINISTIC`

Three things make two runs of a fight differ even with the same input. This switch removes each of them ([`hle_fight_test.c`](../runtime/hle_fight_test.c)):

- **The script random generators** are set to a fixed state at the first battle frame: ssbu-recomp's numbers, with every call count 0. Whatever happened in the menus, the fight starts from the same random numbers.
- **No extra logic frames.** At the end of each frame, the game's graphics module compares the game's refresh count with the count at the last frame, and reports the difference as dropped refreshes. The game then runs that many extra logic frames on the same pad sample. How many depends on when the game's counting thread ran, which is wall time. The hook on that function (`FRAME_END` in [`game_addr.h`](../runtime/game_addr.h)) clears the report after each call, for the whole run.
- **`SSBU_SEQ` counts fight frames.** From the first battle frame on, each poll of player 1 moves the sequence on by the number of fight frames the first fighter has run since the last poll. That can be 0, 1 or more. A step then lands on the same fight frame in every run. After the fight (the game's ready-go flag cleared), each poll is one step again until the next fight.

`SSBU_DET_OFF=<mask>` turns parts off, to see which of them a difference comes from: 1 leaves the generators alone, 2 keeps counting polls, and 4 leaves the dropped refreshes. `SSBU_DETERMINISTIC=0` is the same as unset. ssbu-recomp also seeded the generators from an online match's agreed seed; that is online play, and it is not ported.

What ssbu-recomp measured, without this mode: the same scripted match was not the same fight twice, even at normal speed. From the first battle frame to the end of the match took 2960 frames in one run and 2280 in another, and the fight went apart after about 15 seconds with the same seed (`ssbu-recomp/tests/DEBUG_MODE.md`). The frame line ran 510 times in one run and 605 times in another during the same 462 polls after the first battle frame, which is why the sequence counts fight frames. The generators and the frame end are from ssbu-recomp's reading of the game. With the mode on, the two runs of its fighter-mod test gave the same hash log (see [`SSBU_FIGHT_HASH`](#ssbu_fight_hash)).

Not yet checked in a run:

- that two runs give the same hash log: [the Phase 2 gate](#the-phase-2-gate-the-fight-hash-across-two-runs);
- that the log says the generators were set at the first battle frame, and that the 300-frame lines (with `SSBU_FIGHT_HASH` set) count their calls up from 0;
- that `FRAME_END` is the function ssbu-recomp described, in 13.0.5: the 300-frame lines should count dropped refreshes at `SSBU_SPEED=0`;
- that the sequence follows fight frames: the 300-frame lines say how many fight frames each poll saw.

## `SSBU_FIXED_SEED`

Where the game seeds its random numbers from the clock or from random bytes, they come from n instead ([`hle_fight_test.c`](../runtime/hle_fight_test.c)). Any value that is not empty turns it on, `0` too. n is read by `strtoull` (`0x` for hex), and a value that is not a number counts as 0.

- **Clock reads.** ssbu-recomp found four clock reads whose results the game turns into Mersenne-twister seeds: two of the tick count and two of the time of day. `CLOCK_READ_1` to `CLOCK_READ_4` in [`game_addr.h`](../runtime/game_addr.h) are their return addresses. There, `nn::os::GetSystemTick` and its C name `nnosGetSystemTick` (each wrapped by its exported name) give 0x123456789, and the time of day (`hle_env.c`) gives 1700000000. If the SDK has the two names on one function, that function gets both wraps, one inside the other, and they answer alike. Every other clock read is untouched. The log says the first time each of the four gave its constant.
- **Random bytes.** `nn::os::GenerateRandomBytes` (wrapped by its exported name) and `nn::crypto::GenerateCryptographicallyRandomBytes` (`hle_offline.c`) fill their buffers from a splitmix64 stream that starts from n, as ssbu-recomp's did. The log lists the first 16 calls.
- **The kernel's entropy** (`svcGetInfo` 11, in `kernel.c`) is a function of n and the sub-id. That part is WindowSmash's own: here the SDK's code runs as the game's, where ssbu-recomp replaced those SDK functions.

What ssbu-recomp found: it fixed the random bytes so that a scripted match meets the same random stage and CPU fighter in every run, and it found the clock reads by logging every clock read's caller. A fixed seed alone did not make the fight repeat: the fight still went apart after about 15 seconds (`ssbu-recomp/tests/DEBUG_MODE.md`). That is what `SSBU_DETERMINISTIC` is for.

Not yet checked in a run:

- that the four clock reads happen and give their constant (the log's `fixed seed: the clock read at` lines);
- that two runs list the same first 16 random-byte calls, with the same callers and sizes;
- that the game still runs normally with the entropy fixed: what it does with it was not looked at.

## `SSBU_DEBUG_MATCH`

Sets the fighters, stage and rules of the match, whatever the menus chose ([`hle_fight_test.c`](../runtime/hle_fight_test.c)). The menus still have to be walked to the fight, with `SSBU_SEQ`: they only confirm. When *Ready to fight* is confirmed, the game builds each player's record of the match from that player's choice on the select screen (`PLAYER_RECORD_BUILDER` in [`game_addr.h`](../runtime/game_addr.h)). The hook changes the choice just before the game's function runs, so the record and everything that follows from it are the game's own work. The stage choice is changed at the same time, when player 1's record is built.

Settings are separated by commas. A setting left out keeps what the menus chose. Set but empty (`SSBU_DEBUG_MATCH=`), it is off, as every switch here is.

| Setting | Meaning |
| :--- | :--- |
| `p1=<fighter>`, `p2=<fighter>` | Player 1's or player 2's fighter, by the name in its `ui_chara_` id: `mario`, `donkey`, `kirby`, ... |
| `p3=<fighter>`, `p4=<fighter>` | Two more CPU players. A player 3 or 4 that the menus didn't make a CPU becomes one, with player 2's choice and its own colour. |
| `p2=cpu`, `p2=human` | Player 2's type. |
| `cpu=<1-9>` | The CPU players' level. |
| `stage=<name>` | The stage, by the name in its `ui_stage_` id: `end`, `battle_field`, `mario_castle64`, ... `fd` and `final_destination` mean `end`; `battlefield` means `battle_field`. |
| `form=normal`, `form=battlefield`, `form=omega` | The stage's form. |
| `stock=<n>` | The stocks, in a stock match. |
| `items=on`, `items=off` | The rule's item appearance (2 or 0). |
| `hazards=on`, `hazards=off` | A flag byte that the stage's set-up takes. |
| `time=<minutes>` | Read and not used, as in ssbu-recomp. |

The ids are worked out from the names (the Hash40 of `ui_chara_<name>` or `ui_stage_<name>`), so no list of fighters or stages is kept here. What the game does with a name it doesn't have was not tried. ssbu-recomp's log also named the fighter kind numbers from a bindings file of the game's. That table is game data and is not ported, so the log gives the numbers the game wrote.

The log has a line for each player record, and a line at the game's ready-go with what the match block holds: both fighters' kinds, player 2's type and CPU power, the stage id, form and hazards, and the stock and item rules.

`SSBU_DEBUG_MATCH_LIST=<file>` has one line of settings per match. Each new match set-up in the process takes the next line. A line's settings add to those before it, a blank line keeps the match as it was, and at the end of the file the last match stays. With `SSBU_DEBUG_MATCH` set too, it comes first and the list's lines add to it.

What ssbu-recomp measured (`ssbu-recomp/tests/DEBUG_MODE.md`): three matches set this way, with their fighters, CPU levels, player 2's type and stage in the match block at the game's ready-go. Each ran 1300 fight frames to a normal exit. The form was shown only by the stage id changing with it, and `stock=1` by its effect: the match ended after one KO. Players 3 and 4 got the fighters asked for. Three fights in one process with a list worked. Not verified there: that items then appear, and what the hazards byte does.

Not yet checked in a run:

- that the log has a record line for each player, and a ready-go line with the fighters, stage and rules asked for;
- that the stage choice and the match block are where ssbu-recomp found them, in 13.0.5 (`STAGE_CHOICES`, `STAGE_CHOICE_INDEX`, `STAGE_HAZARDS` and `MATCH_BLOCK` in [`game_addr.h`](../runtime/game_addr.h));
- players 3 and 4, a list of several matches, `items` and `hazards`.

## `SSBU_DEBUG_END`

`<n>s` ends the run after n seconds of fight, which is n × 60 frames; `<n>` ends it after n frames ([`hle_fight_test.c`](../runtime/hle_fight_test.c)). The frames counted are frames shown while the game's ready-go flag is up (in the game's FighterManager, where ssbu-recomp found it), and the count starts again at each fight. With `SSBU_SPEED` or `SSBU_DETERMINISTIC` set, a frame shown should be one game frame. When the count reaches n, the log says `[fight] SSBU_DEBUG_END: <n> fight frames at frame <m> shown: the run ends`, the hash log is written out, and the program ends with exit code 0, the way it does when its window is closed: `ExitProcess`, which runs no `atexit` handlers or libraries' destructors while the game's threads are still running mid-fight (on Linux and macOS the netplay relay's port mapping is removed first, if it made one). The log also has a line when each fight starts and ends.

What ssbu-recomp measured: runs ended this way at the count asked, with exit code 0. That was 1300 fight frames for each of the three matches above, and 1200 for the third fight of a list run (`ssbu-recomp/tests/DEBUG_MODE.md`). ssbu-recomp ended as a closed window does, as here.

Not yet checked in a run:

- that the run ends at the count asked, with that line and exit code 0, on each OS;
- that the ready-go flag is down in the menus and up in a fight (the log's `fight 1 on` and `fight off` lines).

## An example run

A made-up sequence for a run (its draw counts are made up too, so find yours with `SSBU_SEQ_LOG`):

<!-- example sequence: tests/test_test_runs.py parses it -->
```text
# wait for the opening movie, then for the next screen to hold still
@movie
@draws:40-60/30
# a second's pause, then into the menu
60:A
@draws:100-300/30
30:DOWN/4 10:A
# watch for ten seconds
600
```

Saved as `run.seq`, it runs at four times normal speed with the movies cut short:

```sh
SSBU_SPEED=4 SSBU_SKIP_MOVIES=1 SSBU_SEQ="$(cat run.seq)" SSBU_SEQ_LOG=60 ./build/ssbu.exe dump/modules > run.log 2>&1
```

The log then has the `[seq]` lines of each step, the `[display] speed:` line every second and the `[movie]` line of each movie's end.

## Comparing two logs

[`tools/dev/fight_hash_diff.py`](../tools/dev/fight_hash_diff.py) compares two hash logs over the lines both have, since two runs end a few frames apart. Each line is paired with the other log's line of the same frame and fighter, so a line one log lacks moves nothing after it. It prints the first line that differs, with its frame, the fighter's entry id, the fields that differ and the lines around it. Or it says how many lines are the same:

```sh
python tools/dev/fight_hash_diff.py run1.fh.txt run2.fh.txt
```

```text
run1.fh.txt vs run2.fh.txt: first difference at line 2417 of 3214 / 3208: frame 1209 fighter e 1: fields x rngcalls rng
```

Only whole hash lines are read, so a cut last line and the game's other output (with `SSBU_FIGHT_HASH=1`) are left out; where a log lacks lines the other has before its end, the tool says how many it left out. When only `H` differs, what differs is something the lines don't show: the generators' whole hash (a line has it folded to 32 bits), on that line or one before it, or the order of the lines before it. The exit status is 0 when the logs are the same, 1 when a line differs (or no frame and fighter is in both), and 2 when a log is missing or has no hash lines.

## The Phase 2 gate: the fight hash across two runs

The gate is passed when two runs of the same scripted match give the same hash log. Nobody has run it yet. The recipe:

1. Build as usual. Player 1 is the sequence's; unplug the other pads, so that players 2 to 8 get no input.
2. Write the sequence, `gate.seq`. Your steps to *Ready to fight* go first: their draw counts are your build's, so find them with `SSBU_SEQ_LOG` ([an example run](#an-example-run) shows how). The fighters and stage you pick don't matter, as `SSBU_DEBUG_MATCH` replaces them. Then the fight, as below. From `@fight` on, a step is a fight frame, so the input lands on the same frame in both runs. The wait of 520 fight frames is for the countdown, as in ssbu-recomp's own sequence for this test (`ssbu-recomp/tests/seq_det_play.txt`). The input itself is made up.

<!-- gate sequence: tests/test_test_runs.py parses it -->
```text
# your steps to "Ready to fight" go here
# the fight: from the first battle frame on, a step is a fight frame
@fight
520
60:SRIGHT/50 10:A 25:A 40:SLEFT/50 10:B 40:X 30:SLEFT/30 10:A 40:SRIGHT/20 5:A 60:Y/4 20:A 40:SDOWN/20 10:X
60:SRIGHT/50 10:A 25:A 40:SLEFT/50 10:B 40:X 30:SLEFT/30 10:A 40:SRIGHT/20 5:A 60:Y/4 20:A 40:SUP/6 10:X
60:SRIGHT/50 10:A 25:A 40:SLEFT/50 10:B 40:X 30:SLEFT/30 10:A 40:SRIGHT/20 5:A 60:Y/4 20:A 40:SDOWN/20 10:X
60:SRIGHT/50 10:A 25:A 40:SLEFT/50 10:B 40:X 30:SLEFT/30 10:A 40:SRIGHT/20 5:A 60:Y/4 20:A 40:SUP/6 10:X
```

3. Run it twice, each run with a fresh copy of your save (`SSBU_DATA_ROOT`), then compare:

```sh
export SSBU_SPEED=0 SSBU_SKIP_MOVIES=1 SSBU_FIXED_SEED=1 SSBU_DETERMINISTIC=1 SSBU_DEBUG_END=20s
export SSBU_DEBUG_MATCH="p1=mario,p2=donkey,p2=cpu,cpu=3,stage=end,form=normal,stock=3,items=off"
export SSBU_SEQ="$(cat gate.seq)"
for run in run1 run2; do
    rm -rf "$run" && mkdir -p "$run" && cp -R save "$run/save"
    SSBU_DATA_ROOT="$run" SSBU_FIGHT_HASH="$run.fh.txt" ./build/ssbu.exe dump/modules > "$run.log" 2>&1
    echo "$run: exit code $?"
done
python tools/dev/fight_hash_diff.py run1.fh.txt run2.fh.txt
```

The gate is passed when:

- both runs end with exit code 0, after the `SSBU_DEBUG_END` line in their logs;
- both logs say that the generators were set to a fixed state and that the entry id was found;
- `fight_hash_diff.py` exits with 0 and says the logs are the same over at least 2400 lines: two fighters, for 20 seconds of fight after the countdown.

If they differ, the first differing line says at which fight frame and in which fields. A third run tells whether one of the two was odd, and `SSBU_DET_OFF` turns one part of the deterministic mode off at a time. Two runs on different PCs or OSes would be a stronger test of the same thing; that has not been tried either.

## The code

[`game_addr.h`](../runtime/game_addr.h) has the game addresses that the fight switches use, for 13.0.5. Each has the 13.0.4 address it came from beside it, and `tests/test_game_addr.py` checks them against ssbu-recomp's table. [`input_seq.h`](../runtime/input_seq.h) has the steps hook and the marks. [`fight_test.h`](../runtime/fight_test.h) has the fight switches' logic and [`hle_fight_test.c`](../runtime/hle_fight_test.c) their hooks. The frame line and the frame end are wrapped in [`hle_each_frame.c`](../runtime/hle_each_frame.c), whose frame hooks ([`frame_hooks.h`](../runtime/frame_hooks.h)) `hle_fight_test.c` registers on, as rollback will. [`tools/dev/testruntest.c`](../tools/dev/testruntest.c) checks the parser, the step machine, the arithmetic of the speed and the movie count, and the address mapping. [`fighttest.c`](../tools/dev/fighttest.c) checks the fight switches' logic, and [`fightruntest.c`](../tools/dev/fightruntest.c) runs their hooks on made-up guest memory. [`tools/mac/shimtest.c`](../tools/mac/shimtest.c) checks the kernel's entropy (`svcGetInfo` 11) on its made-up game, with `SSBU_FIXED_SEED` and without.

None of the six hooks has a code signature yet. By address: `HLE_WRAP(71038701e0)` (`FRAME_END`) and `HLE_WRAP(71017880c0)` (`PLAYER_RECORD_BUILDER`); `python tools/recomp.py wrap-sig dump/exefs/main <address>` prints the line to put above each, from the game. By exported name: the fighters' frame line, `_ZN7lua2cpp16L2CFighterCommon31sys_line_system_control_fighterEv` (`python tools/recomp.py wrap-sig dump/nro/<lua2cpp_common's .nro> <name>`), and in the SDK `_ZN2nn2os13GetSystemTickEv`, `nnosGetSystemTick` and `_ZN2nn2os19GenerateRandomBytesEPvm` (`python tools/recomp.py wrap-sig dump/exefs/sdk <name>`). `tests/test_wrap_signature.py` lists all six as waiting until then.

Not ported, as ssbu-recomp's research aids or online play: `SSBU_FIGHT_HASH_WORK`, `SSBU_WORK_RESEARCH`, `SSBU_TICK_CALLERS`, `SSBU_FRAMELOG`, the debug match's `SSBU_DEBUG_KEEP`, `SSBU_DEBUG_P34`, `SSBU_DEBUG_RESEARCH`, `SSBU_DEBUG_DUMP`, `SSBU_DEBUG_SCAN`, `SSBU_DEBUG_WATCH` and its other dumps, and the generators' seed from an online match.
