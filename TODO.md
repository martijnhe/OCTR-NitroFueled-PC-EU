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
