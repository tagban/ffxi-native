# The overlay: a cleaner interface over the game

A layer the game host draws over FINAL FANTASY XI: windows that show what the game knows, more
clearly, at the screen's full resolution, and that can stand in for the game's own. **No
automation, by design, on every server: the overlay does something only when the player clicks or
types it, one command per action, through the game's own command parser** (the one its input line
and its own menus use), exactly as if they had typed it.

## What it is made of

```
 game (translated FFXiMain)  ──draws──▶  frame (1080p)  ──MetalFX──▶  screen (4K)
        │                                                              ▲
        │ packets (ws2.c, our Winsock)                                 │ drawn last, at every pixel
        ▼                                                              │
 packet decoder ──▶ game state (zone, you, party, target, effects, chat) ──▶ overlay windows (ImGui)
                                                                               ▲
                                                           mouse and keys ─────┘ (first dibs, then the game)
```

1. **Drawing: Dear ImGui** (MIT). Windows that move, resize, dock and remember where they were;
   text in a real font at any size; tables and bars. Its official renderers cover all three of the
   host's back ends (Metal, OpenGL, Direct3D 12) and SDL3 input. Ashita v4's addons draw with it too.
   The overlay is drawn in the present pass, after MetalFX, so it is sharp at the screen's own
   resolution whatever the game's frame is.
2. **Input.** Mouse and keys reach the overlay first when it wants them (over one of its windows,
   typing in its search box); everything else goes to the game unchanged. Cmd+U on macOS (macOS keeps F11
   for Show Desktop, and the game uses no Command key), Ctrl+Shift+U on Windows and Linux, shows or
   hides the whole layer; F12 stays the launcher's settings.
3. **Game state, from packets.** The host is the game's network layer (ws2.c), so every packet
   passes through it. Zone packets are Blowfish-encrypted with a key derived from the session key
   the sign-in sets (MD5 of it, bumped at each zone change), and the server's are compressed; the
   host knows the session key, so it can read them, the way the client does, without touching the
   game's code, on every game build alike. Written from the documented format: LandSandBoat is GPL,
   and its code is not copied here.
4. **Game state, from memory**, where a packet does not carry it (the camera, the map view): the
   host reads the game's memory directly; the addresses go in each build's metadata, like the
   nameplate hook's.

## Phases

| Phase | What | Done when |
| --- | --- | --- |
| 1. Foundation | ImGui in the host on Metal, drawn after MetalFX; its show/hide key; input routing; layouts kept in the data folder; a first window with what the host knows now (frame rate, frame and screen sizes, in the world or not) | windows drag and resize over the game at 4K and 60 FPS, and clicks go to the right place |
| 2. Packets | the zone key, Blowfish, the server's compression; a game-state module with the zone, the player (position, HP, MP, TP, job), party and alliance, target, status effects, chat | the log shows the right values while playing, zoning, and in a party |
| 3. First windows | party and alliance (HP, MP, TP bars, buffs), target (name, HP, distance), chat (tabs, search, timestamps, links kept); a minimap from the zone's own data (from XI Test Client's work) | on MogHouse, used for an evening's play |
| 4. Other back ends | the same on OpenGL (Linux) and Direct3D 12 (Windows) | the testers' machines |
| 5. Addons | a read-only scripting layer (Lua) for community windows: game state in, ImGui out, nothing sent | an addon written by someone else |

## Where it stands (2026-09-28)

- **Phase 1 done**: ImGui over the game on Metal, after MetalFX, crisp at 4K; Cmd+U / Ctrl+Shift+U;
  clicks and keys routed; layout kept.
- **Phase 2 underway**: three read-only hooks, found in each build by the byte patterns Ashita
  publishes (Ashita's own file is not copied; the addresses go in meta/builds.json):
  - `packet_in`: the success return of the game's decrypt-and-decompress: every server packet,
    readable. Parsed now: zone-in (0x00A), party (0x0DD, 0x0DF, 0x0C8), who is around (0x00D, 0x00E).
  - `packet_out`: the entry of its encrypt: the player's own packets in the clear (their position,
    0x015).
  - `chat_add`: the entry of its add-a-line-to-the-log: every line the log shows (NPCs, system,
    battle, chat), with its mode. The game's own event-script interpreter was also found
    (0x100bc290), for dialogue later.
- **The player's commands**: `input_line` ("addresses"), the game's parser of a typed line
  (cdecl: the line, and 1 for typed). Its own menus build lines such as `/magic "%s" %d` and run
  them through it; the overlay's chat box does the same.
- **Windows**: Overlay (which windows, text sizes), Chat (tabs by kind, words, the game's colors
  changeable, auto-translate phrases, a box to send from), Party (compact, like the game's), Map
  (a radar: who is around, a compass, facing up or north up), Performance.

## Menus: decided

The choice was between (1) the game's menus drawn sharper, (2) display-only companion windows, and
(3) replacement windows that act. **3, with 1 and 2 where they fit**: the overlay's windows may
stand in for the game's, and each is a switch, so a player can keep the game's own interface or
use the overlay's, piece by piece. Every action goes through `input_line` as the player's own
command, one per click, never on its own.

Next, in order: hiding the game's own window where the overlay's replaces it (party list, chat log),
the zone's map art under the radar, nameplates in the overlay's fonts (from the game's view and
projection, which the host sees in its Direct3D calls), then equipment and inventory.

## Rules

- Nothing on its own: the overlay sends a command only when the player clicks or types it, and only
  through the game's own command parser; it writes no packet of its own and presses no key. Addons
  (phase 5) stay read only. Servers can ask players to turn it off; the launcher can do that per
  account.
- It adds nothing when hidden: the game runs as without it.
- No Square Enix bytes in the repository, as everywhere: fonts are open-licensed, map data comes
  from the player's own install at run time.
