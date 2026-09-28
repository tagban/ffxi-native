# The overlay: a cleaner interface over the game

A layer the game host draws over FINAL FANTASY XI: windows that show what the game knows, more
clearly, at the screen's full resolution. Display only: **the overlay never sends anything to the
server and never presses keys for the player.** No automation, by design, on every server.

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
  clicks and keys routed; layout kept. The Status window.
- **Phase 2 underway**: three read-only hooks, found in each build by the byte patterns Ashita
  publishes (Ashita's own file is not copied; the addresses go in meta/builds.json):
  - `packet_in`: the success return of the game's decrypt-and-decompress: every server packet,
    readable. Parsed now: zone-in (0x00A), party (0x0DD, 0x0DF, 0x0C8).
  - `packet_out`: the entry of its encrypt: the player's own packets in the clear.
  - `chat_add`: the entry of its add-a-line-to-the-log: every line the log shows (NPCs, system,
    battle, chat), with its mode. The game's own event-script interpreter was also found
    (0x100bc290), for dialogue later.
- **Windows**: Chat (tabs by kind, words, the game's colours changeable, auto-translate phrases
  from the install's dictionary), Party (HP, MP, TP, jobs).

## Menus: the decision to make

The game's menus (equipment, inventory, status, magic, the auction house...) are the harder part,
and the rule above matters most there. Three ways, which can be combined:

1. **The game's menus, looking better.** They stay the game's (all their logic, nothing new sent),
   drawn sharper: higher-resolution interface textures and fonts (the host's `--textures` packs and
   DAT overlays already do this), the interface at its own scale (the menu resolution). No risk.
2. **Companion windows, display only.** New windows beside the game's menus that only show:
   inventory across every bag with search and sort, equipment sets, what a piece of gear does,
   status effects with their time left, the party's buffs. The game's menus stay for doing things.
   Within the rules as they are.
3. **Replacement menus that act.** New windows that also do things (equip this, use that, move
   an item). Every action would go through the game itself, as the player's own command
   (the same as typing `/equip` in the game's input line), one per click, never on its own: no
   automation. But it is no longer display only, so it needs the rule changed for it, and servers
   told what it does.

The suggestion: 1 and 2 now; 3 only after deciding it, menu by menu.

## Rules

- Display only: no packet is sent, no key pressed, no game memory written, by the overlay or any
  addon. Servers can ask players to turn it off; the launcher can do that per account.
- It adds nothing when hidden: the game runs as without it.
- No Square Enix bytes in the repository, as everywhere: fonts are open-licensed, map data comes
  from the player's own install at run time.
