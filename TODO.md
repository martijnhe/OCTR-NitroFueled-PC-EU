# TODO

Open work for this fork (EU edition and ultrawide). Written so a person or an AI assistant can
pick an item up without the history; addresses are image offsets (no 0x400000 base) unless noted.

## Ultrawide: keep videos, cutscenes and the title screen at 16:9

**State:** ultrawide works in gameplay (scripts/patches.py `OUTPUT_SIZE_SIGNATURE`; the game draws
at the monitor's size and the camera follows its aspect). Three things look wrong at 21:9:

1. **Bink videos** (Activision/Beenox logos, `*.bk2`): stretched sideways to fill the width.
2. **Title screen** (the 3D "Press X to begin" set): built for 16:9, so the sides show the void and
   sky beyond the set.
3. **In-engine cutscenes** (adventure mode, `h100_hub_*_intro.pak` and friends): staged for 16:9,
   expected to show off-stage areas like the title screen. Not yet seen on screen.

**Wanted:** these at 16:9 with black bars (pillarbox), gameplay at 21:9. Optional setting.

**Plan:** the game keeps rendering at the ultrawide size; the presenter
(gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp) shows the frame differently while a
"16:9 moment" is active:

- video: squeeze the full frame into a centered 16:9 rectangle (the video was stretched to fill);
- title screen / cutscene: crop the centered 16:9 part of the frame (the 3D view is correct, only
  wider than the set).

**Signals found so far (EU 1.00 executable; re-find them for 1.21, the code moves):**

- Videos: the game opens `archives/*.bk2` when one starts (`BB_FILE_TRACE=1` shows
  `octane_intro_logo_atvi.bk2`, `openinglogo.bk2`). Track open handles to `.bk2` files in
  src/runtime_file.c; a video plays while one is open. The Bink audio stream (an s16 audio port
  opened by thread `Bink Snd`) is a second signal.
- Cutscenes: `CCutsceneManager` (Alchemy reflection). Its field-name table is at 0x23eb980
  (39 names), field offsets at 0x1c85510 (u16 each). Relevant fields: `_activeCutsceneData` +0xa0,
  `_activeDesignerCutscene` +0xa8, `_paused` +0x59, `_camera` +0x60. The static `_Instance` is very
  likely the global at 0x26b5c68 (right after the class's metafield storage 0x26b5b30..0x26b5c60;
  read by a cluster of functions around 0x98a000). At the title screen `_Instance` was non-null and
  both active fields were 0. Not yet confirmed during a playing cutscene: start a new adventure on
  an empty save slot (the Oxide intro plays) and watch +0xa0 change.
  How to read guest memory from outside: the game image is mapped at 0x800000000 in the
  `sacredfire` process; /proc/PID/mem is readable by a parent process (ptrace_scope 1).
- Title screen: no signal yet. Ideas: the start screen's camera object, or "no player kart and no
  cutscene" from game state; `Octane_StartScreen.pak` loads at boot and stays loaded, so file
  opens do not mark it.

**Then:** the runtime exposes the state (e.g. an atomic the presenter reads each frame, as
BbOverlay does with its settings); a bbport.ini key `ultrawide_cinematics=pillarbox|full` and a
menu checkbox; tests for the patch side.

## Ultrawide: title screen logo and "Press X to begin"

**State (EU 1.21):** src/runtime_frontend.c gives the title screen and the kart customization
screen the horizontal field of view of 16:9 (title 79.755 -> 63.742 degrees, customization
36.739 -> 27.762 at 3440x1440), so their 16:9 backdrop fills the width. The menus are 2D and keep
their ultrawide layout. On the title screen the 3D logo grows by about 1.34 and its bottom
("Nitro-Fueled") now overlaps the 2D "Press X to begin". Narrowing less does not help: the backdrop
covers only 75.5% of the ultrawide width (16:9 is 74.4%).

**Wanted:** the logo a little higher on the title screen only (or the text lower).

**How the field of view works (found 2026-10-16 with a hardware watchpoint on the render camera
and Ghidra):** the main render pass (`CPreRenderUpdatePass`, `_cameraName` "main",
`_renderCamera` +0x168) has a render camera with the horizontal field of view in degrees at +0x3c,
the aspect at +0x40, near/far at +0x64/+0x68 and the position at +0xa0. One function (0xaf7110 in
EU 1.21) writes them each frame: +0x3c = camera setup's radians (+0xd0) * 57.29578 (the
instruction at 0xaf7278, which runtime_frontend.c points at its own factor), +0x40 = window
width/height * `_cameraAspectRatioMultiplier`. Readers (0x355c5c frustum, 0x960a04, 0xb0363b)
take tan(fov/2): the sideways extent is tan, the vertical tan/aspect (so +0x3c is horizontal).
Next: find where the position (+0xa0) or the view direction is written in the same way (a
watchpoint on +0xa0) and lower the camera a little while the title screen shows.

## Online (RyuNET) on the European edition

**State:** Ryu7w7/OCTR-NitroFueled-PC v0.5 is merged (commit "Merge upstream v0.5"). Offline the
EU game runs as before. Online is off by default (`online=1` and `online_name=` in bbport.ini, or
the launcher). Tested on EU 1.21 (2026-10-16): sign-in and online play on the RyuNET server work.

**How upstream built it:**
- v0.3 (online): a replacement Demonware server (RyuNET, `octr.ryu7w7.xyz`, auth over HTTP on
  port 8080). The game checks Demonware's signed answers with an RSA-2048 key in its executable;
  `patches/CrashTeamRacingOnline.xml` (`tools/make_online_key_patch.py`) swaps it for the server's.
  New runtime: sockets and DNS (`runtime_net.c`, `runtime_net_overrides.c`), HTTP
  (`runtime_http.c`), a PSN stand-in (`runtime_np_online.c`: account id from the nickname, sign-in
  state, tickets), `run.py setup_online` (server, nickname, key file `ryunet_online.key`).
- v0.4: private matches and groups (session create/join/leave and invitations answered by
  `runtime_services.c` as PSN WebApi responses, session ids `cusa13795-session-<name>`), the
  friends list (`sceNpWebApiReadData` returning the transferred length; the user service's
  initial login event for user 1).
- v0.5: nicknames owned by an installation key and linked to Discord (Windows launcher
  `discord_link.cpp`), Discord invites and presence, volume (`runtime_audio.c`), keyboard as a
  split-screen player (`runtime_pad.c`), LAN address reported to the game (players behind one
  router), error code 3 (memory committed on use, Windows) and 23 (shader float-to-int rules,
  loop limits).

**Done for EU:** `patches/CrashTeamRacingOnlineEU.xml` (the same key; the Demonware key is at
image offset 0x1becf58 in EU 1.21, 0x40 before the USA 1.21 one, found by signature); friends'
presence names the running title id.

**Open:**
- Test with a USA player: private rooms and races across the two editions (Demonware title ids
  differ per region; the EU executable's own title id is sent at sign-in).
- The Windows launcher (`launcher/windows/launcher.cpp`) still names CUSA13795 for the game
  folder, saves and languages.
- `build.sh --test`: upstream's new code fails the tests' -Werror on format truncation
  (`runtime_services.c`, `runtime_net_overrides.c`); the game build itself passes.

## Discord on Linux and the Steam Deck

Upstream's Discord parts are Windows-only (`launcher/windows/discord_link.cpp`,
`discord_presence.cpp`, in the Windows launcher). On Linux the nickname can only be kept by keeping
`ryunet_online.key`.

- **Nickname link / recover / unlink** (an OS reinstall or another PC keeps the nickname): OAuth2
  with Discord app 1560408801704943708, scope identify, redirect `http://127.0.0.1:<53134-53136>/discord`
  (registered at Discord); the code goes to the RyuNET server, `POST /ryunet/discord/link`
  (nick, code, redirect_uri; nick empty = recover), `POST /ryunet/discord/unlink` (nick),
  `GET /ryunet/nick?name=` (status). Every request carries `X-RyuNET-Key: HMAC-SHA256(key, host)`
  (run.py server_key). Done (2026-10-16): `scripts/discord_link.py` (status, link, recover, unlink)
  and the in-game menu's Online section (Link / Recover with Discord, Status); link tested. Recover
  not tested yet (it moves the nickname to the recovering installation's key). Still open: the
  buttons in the Linux GTK launcher.
- **Presence and invites**: done (2026-10-16): `scripts/discord_presence.py`, started by run.sh
  next to the game when online is on (the IPC socket `$XDG_RUNTIME_DIR/discord-ipc-N`, also
  Flatpak and Snap Discord). The presence and the connection are tested; accepting an invite
  (ryunet_join.txt, the host as a friend) needs a second player to test.

## Grand Prix intros and CTR TV episodes (optional)

**State:** the menu's *CTR TV* plays a CTR TV episode in place of the opening video
(`openinglogo.bk2` redirected in src/runtime_file.c, BB_CTRTV=<episode>); it lists every
`CTRTV_*.bk2` in the game folder's archives. 1.21 (EU and USA) has only `CTRTV_FebGP.bk2`; the
start screen (`octane_startscreen.pak`) has only Crash's intro (`Crash_MainMenu`,
`Cutscene_CrashMainMenu`); 1.00 shares its opening videos with 1.21.

**Wanted:** the earlier seasons' intros (updates 1.10 to 1.20: their CTR TV episodes and the
start-screen intros where another character jumps into the logo, see "Intro Comparison + All
Grand Prix Intros" on YouTube). Reports come in through the issue form
`.github/ISSUE_TEMPLATE/grand_prix_intro.yml` (names, sizes, checksums; no game files). Then:
check whether a start-screen intro is a `.pak` cutscene the game picks by Grand Prix data
(`CCtrTvComponentData` `_CtrTvIntroName` / `_CtrTvGPName`, CGrandPrixSystemData) and add a menu
choice for it.

## Ultrawide on USA 1.21 (CUSA13795)

`OUTPUT_SIZE_SIGNATURE` was taken from the EU 1.00 executable. On another build it applies only
where the signature matches once; otherwise patches.py logs "patch site ... not found" and skips
it. Check it on a USA 1.21 dump and on EU 1.21.

## Pit Stop offline (in progress)

The Pit Stop is a Demonware marketplace (`CMarketplace*` tasks: GetBalances, GetInventory,
DepositCurrency, ConsumeInventoryItems, Exchange...; host `cert.uno.demonware.net`): the Wumpa
Coin balance and owned items live on Activision's servers, and Demonware login needs a PSN
sign-in. The port answers PSN and network calls with "signed out", so the game shows "You are not
connected to the internet" and locks the Pit Stop. Connecting to the real servers is out of scope.

Plan: run the marketplace locally. Either patch the game's marketplace tasks to complete with
local results (balance and inventory kept in a file next to the save), or answer its Demonware
requests in the runtime. First steps: find where the game decides it is offline for the Pit Stop
(`checkIfMarketplaceIsReady_VSC`, `CContentDeploymentStateOffline`), and how a task reports its
result; check whether the catalogue (`CMarketplaceExchangeSkuData`, featured-SKU calendar) is in
the game data or only on the server.

### Findings (EU 1.21, BB_ONLINE=local diagnostic mode, src/runtime_online.c)

- With the network up and PSN signed in locally, the game asks for a PSN ID token
  (sceNpAuthGetIdTokenA) and POSTs Demonware **auth3**:
  `https://lavender-ps4-auth3.prod.demonware.net:443/auth/` ("lavender" = this title),
  body `{"auth_task":"46","iv_seed":"...","title_id":"5760","identity":"356c4bc3",
  "extra_data":"{\"version\":\"0\",\"token\":\"{...id_token...}\",\"extended_data\":true}"}`.
  It retries 6 times on an error answer.
- A success answer carries `client_ticket`/`server_ticket` (encrypted with a title key and the
  iv_seed); strings `X-Signature`, `DW-RSAENC`, `public_key`, `ciphertext` suggest the answer is
  signed/encrypted against a key in the executable: a local answer needs that key replaced or the
  check patched (patches.bin can do either).
- After auth the game uses the LSG (classic binary Demonware over TCP, session-key encrypted):
  `LSGConnect`, `LsgAuthenticateTicket`, `GetContentPublisherFile` (the publisher config that
  drives Grand Prix), `ReadAllKeyArchive`, leaderboards, storage files. UNO REST endpoints
  (`%s/v1/users/%s-%llu/...`, abtesting, friends, loot-drop) also exist.
- Conclusion: emulating Demonware means auth3 + the encrypted LSG protocol + the marketplace
  services: large. The smaller route for an offline Pit Stop is a game-side patch: make the
  marketplace's tasks (CMarketplaceGetBalancesTask, ...GetInventoryTask, ...DepositCurrencyTask,
  ...Exchange...) complete locally with a coin/inventory file next to the save, and its
  "ready/online" checks pass.
- Online racing on community servers would need the above plus matchmaking and peer connections
  (NpMatching2 / Demonware lobbies): a separate project.

### Offline Pit Stop: map of the EU 1.21 code (image offsets; patch Address = offset + 0x400000)

- The marketplace is not on the UNO REST API (no inventory/balance URLs among the templates):
  it runs over the LSG. So the route is a game-side patch, not JSON answers.
- `CMarketplace` (singleton pointer at global 0x2ac8348; field table 0x2674890):
  +0x18 `_data` (CMarketplaceData, from looseData/MarketplaceData.igz in permanent.pak: items,
  SKUs, prices, featured calendar, bundles: the catalogue is local), +0x28 `_config`,
  +0x40 `_inventoryItems`, +0x48 `_balances`, +0x64 `_loadedElements` (bit 1 inventory, bit 2
  balances; 3 = ready), +0x98 `_purchasedExchangeSkuIds`.
- Server task callbacks: 0xab4470 inventory arrived (task+0xa0 -> +0x40, sets bit 1, calls
  0xab35d0), 0xab4360 balances arrived (task+0xa8 -> +0x48, sets bit 2, calls 0xab35d0);
  static thunk 0xab4450(task) calls the inventory one on the singleton.
- `CMarketplaceNodes` visual-script functions (registered at 0x193e21c, 14 in order):
  clearVisibleItemsList 0x11444b0, fetchWumpaCoinsBalance 0x11444c0 (reads 0xab06c0(...)),
  checkIfMarketplaceIsReady 0x1144500, setSelectedFeaturedIndex 0x11445c0,
  getSelectedFeaturedIndex 0x11445e0, getDailyDealsRemainingTime 0x1144600,
  setNextPitStopRefreshTime 0x1144710, sendVisitPitstopEvents 0x1144720, getFillUpEnabled
  0x1144730, getExtraTabEnabled 0x11447b0, showExchangeResult 0x11447f0, refreshPitstop
  0x1144830, getRemainingRefreshCharges 0x1144e30, automaticRefreshPitstop 0x1144f40.
- checkIfMarketplaceIsReady needs: CMarketplace+0x18 != 0, +0x64 == 3; global 0x2acb3d8 (an
  online/session object): [+0x68]->count at +0xc > 0, ([+0x50] | 1) == 3; then [+0x40]->+0x130
  byte +0x11 or 0xaa2b90(...). Forcing it ready (jmp at 0x1144518 -> 0x114457c) is not enough:
  the main menu's Pit Stop entry shows "A network error has occurred" before the shop opens, a
  separate gate (probably the 0x2acb3d8 session state).
- Hook plan for C code inside the game: redirect a call in the shop code to an import the game
  never uses offline (e.g. sceNpWordFilterSanitizeComment); runtime_online.c implements that
  import, running on the game thread (guest TLS set), and can fill inventory/balances and handle
  purchases from a local file (user/marketplace.json).
