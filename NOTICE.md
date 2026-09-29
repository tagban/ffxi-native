# Notice

ffxi-native is derived from **xi-on-mac** by rubymatrix, <https://github.com/rubymatrix/xi-on-mac>,
used with its author's permission. The recompiler (`recomp/`, `discovery/`), the runtime
(`runtime/`), the game host (`host/`), the per-build metadata (`meta/`, `specs/`), the PlayOnline
client (`launcher/pol/`), the tests and the original launcher come from that project.

`runtime/portable/zonemap.cpp` (the overlay's zone maps) is adapted from the MogHouse client (XI
Test Client, MIT, John Leighow): its DAT, MZB and file-table readers and its walkable-ground raster.
Dear ImGui (`third_party/imgui`) is MIT (Omar Cornut).

`third_party/stb/stb_image.h` is public domain (Sean Barrett). SDL3 (zlib licence) and Mbed TLS
(Apache 2.0) are built into the Linux game host by `tools/linux_dist.py` and bundled with the macOS
app.

FINAL FANTASY XI and PlayOnline are trademarks of Square Enix. This project is not affiliated with,
endorsed by, or supported by Square Enix, and holds none of its code or data: each player's
launcher makes the game from their own install.
