# yuzu-compatible room client (experimental)

This mode connects OpenSmash to an **existing** yuzu-compatible room server. It implements the client only. No room server, hosting service or server deployment is included.

The wire baseline is room protocol 1, as preserved in yuzu-era network/LDN sources at commit 48d6226115f849c465202692b6988e1efb7d2410 in the review's source mirror. Compatibility with a named client version must be confirmed by real matches. Automated tests currently exercise the real ENet and Zstd libraries and OpenSmash's guest network HLE against a loopback test double; they do not prove a complete Smash match with an emulator.

## Build and start (Windows)

1. Build this checkout normally so that build/ssbu.exe includes the new runtime. Updating only the launch script is insufficient. The launcher rejects older executables.
2. Run: `python tools/yuzu_client_deps.py`. This downloads checksum-pinned ENet 1.3.18 and Zstd 1.5.7 sources and builds the optional client DLLs into build/yuzu/. The ordinary OpenSmash mode does not load them.
3. Double-click **launchers/Join yuzu Room.bat**. Enter an existing room server's address and optional UDP port (default 24872), and the room password if needed. Game UDP defaults to the Eden 0.2.0 wire format.
4. In the game, use **Local Wireless**. To join an existing game, open **Rooms List**. To let another client join your game, choose **Create a Room**. Creating this game session does not run a yuzu room server on your PC.

The server address and nickname are kept in config/yuzu_client.json. The password is not saved or written to logs. The initial nickname has a random suffix to reduce name collisions. Change it by running the PowerShell launcher with -Nickname.

PowerShell example (the address is a placeholder):

`powershell -NoProfile -ExecutionPolicy Bypass -File tools/yuzu_client.ps1 -Server room.example:24872 -Nickname MyName`

Eden 0.2.0 and historical yuzu use the same room protocol version but different socket enum values inside game UDP packets. The default `eden` profile sends IPv4=1 and UDP=3, matching Eden 0.2.0. For a historical yuzu peer using IPv4=0 and UDP=2, pass `-ProxyProfile yuzu` to the PowerShell launcher. The profile is saved with the server and nickname. Choose it for the game peer; the server's name or room protocol version does not identify it. Incoming game UDP accepts either complete tuple and rejects TCP and mixed enum tuples. There is no automatic format negotiation or duplicate sending.

For a complete pre-launch check without starting a play session, pass `-NoPrompt -CheckOnly` to tools/yuzu_client.ps1. This validates the executable, client libraries and plugin preparation. It keeps the saved connection settings unchanged and does not connect to the room server.

The launcher disables OpenSmash rollback and run-ahead for this process. The existing OpenSmash lobby, P2P and Quickplay features are not available in this mode. The Online settings page shows the selected client connection; change its server through the client launcher.

For a controlled first match, use game 13.0.5 and match DLC, with mods disabled on both clients. Start with two players. A working room connection alone does not establish game version or mod compatibility.

## Direct environment configuration

These are new variables; existing OpenSmash relay variables are not yuzu server addresses.

| Variable | Meaning |
| --- | --- |
| SSBU_NET_BACKEND=yuzu | Select the optional room client. Leave unset for the existing OpenSmash network. |
| SSBU_YUZU_SERVER=host:port | Existing server; IPv4 address or DNS hostname. Default port 24872. |
| SSBU_YUZU_PROXY_PROFILE=eden | Outgoing game UDP format: eden (default, Eden 0.2.0 INET=1/UDP=3) or yuzu (historical INET=0/UDP=2). The launcher exposes -ProxyProfile and saves it. Incoming UDP accepts either complete tuple. |
| SSBU_NET_NAME | Room nickname: 4-20 ASCII letters, digits, spaces, dots, underscores or hyphens (reference room rules); launcher saves a nickname. |
| SSBU_YUZU_PASSWORD | Room password, optional; never logged or saved by the launcher. |
| SSBU_YUZU_TOKEN | Optional protocol token. Service-specific token acquisition and public directory APIs are not implemented. |
| SSBU_YUZU_LIBDIR | Optional directory containing enet.dll and libzstd.dll; default is yuzu/ beside ssbu.exe. |
| SSBU_RB=0 | Required for mixed clients. A nonzero SSBU_RB is explicitly rejected. |
| SSBU_RUNAHEAD=0 | Disable run-ahead for the initial mixed-client tests. |

The room-assigned virtual IP is exposed to the game. LDN requests use typed room messages; gameplay UDP uses ProxyPacket with Zstd. A lost room connection resets the LDN session and queued game datagrams before retrying. Wrong password, incompatible room protocol, full room, kick and ban are reported without falling back to the OpenSmash lobby. Automatic reconnect restores the outer connection; it does not automatically rejoin a lost game session.

The optional client currently targets Windows. Other platform builds retain native networking and report that this optional mode requires Windows.

## Validation

- python tests/test_yuzu_wire.py: independently constructed golden frames, endian values, protocol enums, truncation and bounded malformed-input checks.
- python tests/test_yuzu_client.py: actual ENet/Zstd/client/HLE on loopback, admission, game identification, discovery, join in both directions, UDP, refusal, liveness and reconnect.
- python tests/test_yuzu_launcher.py: isolated PowerShell preflight, preparation failure, old executable detection, saved profiles and process configuration. Synthetic executables only.
- python tests/test_yuzu_menu.py: real backend in the existing menu; read-only compatibility rows preserve native settings.
- Existing native network tests remain applicable to the default backend. Windows CI builds ENet/Zstd before discovery so the client integration tests run instead of skipping for missing DLLs.

On 2026-11-05, the user reported multiple complete, working matches with Eden (identified earlier in the same test session as 0.2.0) and another yuzu-compatible client after the UDP enum fix. This establishes user-reported real mixed-client match interoperability for those tested clients. The second client's name/version, host direction for each run, player counts, mod configuration and selected UDP profiles were not specified. Track these details for future compatibility coverage; the result does not establish support for every yuzu-derived client.

Logs from the dedicated launcher are build/yuzu-client.log (runtime stderr) and build/yuzu-client.stdout.log. Unless SSBU_NET_LOG is already set, the launcher enables level 1 diagnostics. Bounded metadata traces show LDN Connect/SyncNetwork, outgoing packets submitted to ENet (this does not prove peer receipt), receive filtering, Zstd decoding and delivery to game UDP sockets. Discovery, control and UDP have separate limits so repeated scans do not consume the control trace budget. Game payloads and room credentials are not logged. The overlay and Esc > Online show the connection state. Room credentials are not included, but names, addresses and local file paths may appear in diagnostics.

## Source and licenses

The ENet public headers under third_party/enet/ are from upstream tag v1.3.18, MIT licensed; its LICENSE is preserved there. The dependency builder records the upstream archive checksums and copies dependency license files beside the DLLs. ENet is used as a client transport; its ability to create a listening endpoint is not exposed as a product feature. Zstd sources remain under ignored deps/yuzu-client and are used to build libzstd.dll, with its LICENSE and COPYING copied to the output.

The new codecs and backend implement the client wire format in C, without copying emulator Core or UI components. The detailed design is in YUZU_NETPLAY_FEASIBILITY.zh-CN.md. Public protocol source revisions, URLs and SHA256 hashes are recorded in YUZU_PROTOCOL_SOURCES.json; no emulator source or game data is included in that manifest.

## Local validation on 2026-11-05

The Windows game executable was relinked successfully with 1,657 objects. The new client suite passed 16 integration tests and 7 codec tests. Native client, LAN, settings, lobby menu and rollback regression suites passed 62 tests, with one platform/environment-specific LAN test skipped. New C modules also compiled with /W4 /WX; launcher feature checks and invalid-input checks passed.

These automated results cover local test doubles and existing native regression tests. They do not constitute a real mixed-client match acceptance. The separate user reproduction below records the external Eden test.

## Cross-client test report on 2026-11-05

The user confirmed admission to an existing yuzu room and game-room discovery in both directions. Joining a game timed out with either client hosting; the host showed no visible join request. The peer was identified as Eden 0.2.0. In the captured OpenSmash-hosted attempt, Eden's Connect reached OpenSmash, OpenSmash sent SyncNetwork, and game UDP repeatedly left port 12345 without any inbound game UDP response. The original log is retained locally by the developer; it is not included in the source tree.

Eden v0.2.0 commit ee188168c12188340961314e1dfc70a4f7ad4f7a adds Unspecified to Domain and Protocol in src/common/socket_types.h, shifting INET/UDP from historical yuzu's 0/2 to 1/3. room_member.cpp writes the raw enum values into ProxyPacket, and ProxySocket::HandleProxyPacket ignores a protocol value that differs from its UDP socket. The old outgoing UDP=2 therefore represents TCP to Eden. Room protocol 1 and LDN message layout do not identify this incompatibility.

The client now defaults to Eden's outgoing tuple, preserves a selectable historical yuzu tuple, and strictly accepts either complete incoming UDP tuple. Nine codec tests and 19 client tests pass, including Eden UDP after LDN connection, legacy UDP round trips, malformed/mixed tuples and invalid profile rejection. Public source revisions and SHA256 records are in YUZU_PROTOCOL_SOURCES.json. The subsequent user test below confirmed complete mixed-client matches after this fix.

## Successful mixed-client matches reported on 2026-11-05

After the Eden UDP enum fix, the user reported testing multiple matches with Eden and another yuzu-compatible client, with complete functionality. Eden was identified as 0.2.0 earlier in this test session; the second client was not named. This is a user-reported real match result, separate from the 19 client integration tests and nine codec tests.

| Peer client | Real match result | Evidence scope |
| --- | --- | --- |
| Eden 0.2.0 | Multiple complete matches reported working | User test after the UDP enum fix |
| Another yuzu-compatible client (name/version unspecified) | Multiple complete matches reported working | User test after the UDP enum fix |

Exact match counts, host directions, player counts, latency/loss measurements, mod configurations and selected UDP profiles were not provided. No further compatibility or performance claims are inferred from this report. The implementation remains client-only.

## Integration with upstream main

The client was integrated on upstream main at 078e05533d53100ce89186a9cff61630420e6429. Its launcher uses the current tools/skyline.py build entry point and prefers .venv or the installer's embedded Python. The native backend's peer tracking remains active only in native mode, and compatibility mode disables native default rollback even when native connection variables are inherited. Reconnecting also discards queued guest UDP from the former session.

Local validation on this integration: 20 client tests, nine codec tests, five launcher tests and one menu test passed. The affected native suites ran 83 tests (82 passed, one real-multicast test skipped). The full Windows runtime compiled and linked against synthetic modules without game data, both as a single executable and with the separate game DLL; the latter loaded successfully. Real matches reported above were played on the feature build before integration with this upstream revision; they were not rerun by the agent after integration.

The client and script entry point are delivered together with the necessary existing-menu status adaptation. A graphical connection form, profile editor and connection-management UI are a separate follow-up.
