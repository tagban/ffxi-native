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
- **Windows**: Overlay (which windows, text sizes, fonts), Chat (tabs by kind, words, the game's
  colors changeable, auto-translate phrases, a box to send from), Party (compact, like the game's),
  Target, Map, Performance.
- **Map**: a radar on the zone's map, made when the player zones in (runtime/portable/zonemap.cpp,
  adapted from the MogHouse client). Two styles, switched with a right-click:
  - **The game's own map art**, where the install has one: DATs with a texture named
    `menumap m_<zone>_<map>` (512x512, 8-bit with a palette rows bottom-up, or DXT1/DXT3 blocks
    top-down) and a 0x31 chunk whose quad gives the pixel of the world's origin (minus its first
    corner, plus 6 in x on every map checked) and a byte saying whether it is a field's map. Fields
    are 0.2 pixels a yalm and the rest 0.8, checked by fitting the zone's collision edges to the
    map's ink (Bastok Markets, Port Bastok, Port Jeuno, Southern San d'Oria, East and West
    Ronfaure). 210 maps of 115 zones in the base install; expansion zones' maps use other names,
    not yet found. Zones with several maps (floors) take the best-fitting one for now; choosing by
    where the player is, is to come.
  - **Drawn from the zone's collision mesh**: walkable ground from above, shaded by height, a
    paper's grain, its edges, the ground reachable from the player bright and the rest dimmed;
    tinted with the map colors (papyrus by default).
  Positions and facing come from the game's own entities every frame ("entity_map": position at
  +0x04, facing at +0x18, the server's id at +0x78, the name at +0x7C). Entities the game does
  not draw (status, flags and look, as MogHouse reads them) are left off. Colors for everything on
  it are the player's (right-click, Colors).
- **The game's own windows**: its window manager ("menu_mgr") keeps them in a list (nodes: next,
  the window at +0x10, removed at +0x14; a window's rectangle at +0x3A, its name in [+4]+0x46;
  the one with the keyboard at the manager's +0x54). The log is moved off the screen (the game
  leaves it there); the party list is laid out again every frame, so its draws are dropped
  instead: hooks on the manager's draw of each window ("menu_draw", "menu_drawn").
- **Typing**: with the game's log hidden, Enter (nothing targeted), "/" and "!" open the overlay's
  chat box while none of the game's menus has the keyboard (knocked out, its death menu does not
  count). Space is /jump (a setting; off, Space opens the box too). Up and Down in the box go
  through what was sent. Knocked out, Say, Shout and Yell are held back; Party, Linkshell and Tell go.
- **Knocked out** (the player's server_status 3 in 0x037, the time left from its dead_counter1): the
  game's picture goes grey at the present (gfx_set_grey; Metal), the game's death menu is not drawn
  (its rectangle dropped), and the overlay shows the time and Return to Home Point (Enter on the
  death menu, then Left on its Yes/No until its cursor, +0x4C, is on Yes, and Enter) and Wait for a Raise (a strip at the top).
- **Marks**: GM (and level), mentor, new adventurer, seeking a party, away, bazaar, and the
  linkshell's pearl in its color, from the players' flags (0x00D; the player's own 0x037), drawn
  as vector shapes by names over heads and in the Target and Party windows.
- **Target**: the game's target window ("target_ptr") holds who it shows; which word is found by
  matching while something is targeted.
- **Nameplates**: the game's nameplate routine (the nameplate_scale hook) hands each name, as the
  game placed and colored it, to the overlay, which draws it in its own font; the game's glyphs
  are drawn at no size. Fonts are the system's own (nothing shipped) or the built-in Roboto.

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

## For later: controller play (Steam Deck) as its own way of using the interface

When the windows are designed further, controller play is a first-class choice beside keyboard and
mouse, not an afterthought (the player's wish, 2026-09-29). Steam Deck players in particular:

- A setting, **Controller** or **Keyboard and mouse** (or following the last input used), that
  changes how the overlay behaves, not only how it looks.
- Controller: every overlay window reachable and usable with the pad alone (ImGui's gamepad
  navigation, focus that moves predictably, bigger targets, button hints like the game's own), the
  Actions menu (magic, abilities, trust, items) laid out for a pad the way FFXI's own menus are, and
  the chat usable without a keyboard (auto-translate lists and canned phrases before typing; Steam's
  on-screen keyboard when typing is needed).
- The Deck's screen: 1280x800 at arm's length, so text and windows larger by default there, and
  layouts that fit 16:10.
- FFXI's own pad scheme stays the reference (its menu button, cancel, the target cycle), so the
  overlay never fights what the game does with the same buttons.

## For later: monsters of any size (the player's idea, 2026-09-29)

The server can't: LandSandBoat's `mob:setModelSize(0..3)` goes to the client as `GraphSize`, a 2-bit
field of the entity update (0x00E), which the client turns into one of four preset scales; 3 is the
largest there is. The client can: every MogHouse player runs this launcher, so the step where the game
turns the preset into a scale (a table of scales, or a scale on each entity in memory) can take any
value. Plan: (1) find it and prove it here (a mob 5x); (2) say who is huge: a size list in the
launcher, or, better, a small LandSandBoat module sending the launcher a scale with the entity (read by
the packet hook), so a GM's `!size 5` works live; a plain client just sees the normal size.
