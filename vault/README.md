# xi-vault: game versions for servers and players

`FFXiMain.dll` decides how the DAT files are read, so a **version** is the DLLs and the DATs together.
`xi-vault` records an install as one version (a manifest of every file's path, size and SHA-256) over a
content-addressed store, and never mixes the files of two versions. The launcher uses the same library
to back up the player's install, repair it, and bring the version a server wants.

It holds Square Enix's files. Nothing here ships them: a vault is made from your own install, and what
a server publishes is its operator's choice.

```
cargo build --release          # vault/target/release/xi-vault
export XI_VAULT=/path/to/vault # or --vault <dir>; keep it on the same drive as the install (APFS: no extra space)
```

## Capturing an update

```
xi-vault snapshot "<...>/FINAL FANTASY XI"    # before updating: e.g. version 30260805_0
# ... update the game ...
xi-vault snapshot "<...>/FINAL FANTASY XI"    # after: e.g. 30260903_0
xi-vault diff 30260805_0 30260903_0 [--files] # what changed, by folder
xi-vault pack 30260805_0 30260903_0 30260805_0..30260903_0.tar.zst   # only what's new, compressed
```

The version name comes from `meta/builds.json` when the recompiler knows the build, else
`unknown-<hash>` (give one with `--name`). `USER/` and `TEMP/` are the player's and are skipped.

## Serving versions (a server operator)

Put the client your server wants in a vault (`snapshot` its folder), then publish and serve it:

```
xi-vault publish site --current 30260903_0 30260903_0 --since 30260805_0   # only what changed since 30260805_0
xi-vault serve site                                                         # http://0.0.0.0:54080/
```

- `--since <version>` hosts only the files that are not in that version, a few hundred MB instead
  of 15 GB. Players bring the rest from their own install: the launcher backs it up first and checks
  it is that version, and says so plainly when it is not. Leave it out to host every file.
- `--current` is the version your lobby wants: LandSandBoat's `CLIENT_VER` (`settings/default/login.lua`).
  A client older than it is refused with lobby error 331, which sends the launcher here.
- `--packs` also writes one-file deltas between the versions listed (a player with the older one
  downloads a single compressed file).
- Port **54080**, on the same host as the server, is where the launcher looks when a player gives no
  address: nothing to configure on their side. Open it in the firewall. Any other address works too
  (players put it in the account's **Game updates address**), including a CDN or `https://`.

`site/` is plain static files, so any web server can serve it instead of `xi-vault serve`:

| file | what |
| --- | --- |
| `index.json` | the versions, the one the server wants (`current`), the packs |
| `versions/<version>.json` | each version's manifest (and the `--since` base's) |
| `objects/ab/<sha256>.zst` | every hosted file, by content, zstd-compressed |
| `packs/<from>..<to>.tar.zst` | a delta: only what `<to>` adds, one download |

As a service (`/etc/systemd/system/xi-vault.service`):

```
[Unit]
Description=FINAL FANTASY XI client versions for the launcher
After=network-online.target

[Service]
ExecStart=/usr/local/bin/xi-vault serve /srv/xi-vault/site --listen 0.0.0.0:54080
User=xi-vault
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

What the launcher does on Play: reads `index.json`; if the player lacks `current`, downloads only the
files they do not have (checking every one against its SHA-256), puts that version together beside
their install (theirs is never changed), makes the game for it, and plays.

**Safety.** The launcher only ever compiles a version whose `FFXiMain.dll` and `FFXi.dll` match a
build in `meta/builds.json`; it refuses anything else. A site that is compromised, or a connection
that is tampered with, cannot get the launcher to run code it does not already know. A new client
version needs its metadata in the launcher first (`.claude/skills/game-version-update`).

## Other commands

```
xi-vault list                                  # versions in the vault
xi-vault verify <folder> <version> [--full] [--repair]
xi-vault materialize <version> <out folder>    # a version as an install of its own
xi-vault fetch <url> [--version v]             # from a published site into the vault
xi-vault unpack <pack.tar.zst>
```
