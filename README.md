# SilencedTracerBlocker

**Cross-platform source package: Linux + Windows.**

A minimal MetaMod:Source plugin for Counter-Strike 2 that targets **silenced weapon tracer particles only**.

This version intentionally does **not** block:

- `bullet_impact`
- wall impact decals/effects
- blood impacts/effects
- `player_hurt` / damage events
- non-tracer particles

## How it works

1. Listens for CS2 `weapon_fire` events.
2. If the event says `silenced = true`, or if the weapon is `weapon_mp5sd`, it opens a very short tracer-block window.
3. Detours the server-side particle dispatch function from gamedata.
4. Blocks only particle names that look like weapon tracer particles, such as `weapon_tracers`, during that short window.
5. Lets all bullet impact, blood, damage, and non-silenced weapon events continue normally.

## Important notes

The included Linux signature came from the files you provided:

```txt
DispatchParticleEffect_Linux = 55 48 89 E5 41 57 41 56 41 55 41 54 41 89 CC 53 48 89 D3
```

The Windows signature was extracted from the `server.dll` you uploaded. It resolves to the likely `DispatchParticleEffect` function at RVA `0x3CB7D0` in that DLL.

```txt
DispatchParticleEffect_Windows = 48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 20 4C 89 44 24 18 55 41 54 41 55 41 56 41 57 48 8D AC 24 80 FE FF FF 48 81 EC 80 02 00 00 33 FF
```

Update both signatures after CS2 server updates if the pattern stops resolving.

## Windows signature note

The Windows detour patch length is set to 15 bytes so the trampoline returns on an instruction boundary for the uploaded `server.dll`. Do not change this back to 16 on Windows unless you re-check the function prologue.

## Build on GitHub / GitHub Actions

1. Create a GitHub repo.
2. Upload the contents of this ZIP to the repo root.
3. Open **Actions**.
4. Run **Build SilencedTracerBlocker**.
5. Download the artifacts:
   - `SilencedTracerBlocker-linux`
   - `SilencedTracerBlocker-windows`

## Install

After GitHub Actions finishes, download the matching artifact for your server OS and copy the built package contents into your server `game/csgo` folder.

Expected layout:

```txt
addons/metamod/SilencedTracerBlocker.vdf
addons/SilencedTracerBlocker/bin/linuxsteamrt64/SilencedTracerBlocker.so
addons/SilencedTracerBlocker/bin/win64/SilencedTracerBlocker.dll
addons/SilencedTracerBlocker/gamedata/silenced_tracer_blocker.games.txt
```

Then restart the server and run:

```txt
meta list
```

## Updating signatures

If the plugin stops loading after a CS2 update and says the signature was not found, update:

```txt
addons/SilencedTracerBlocker/gamedata/silenced_tracer_blocker.games.txt
```

## License

MIT

## Build note

This package includes a small `network_connection.pb.h` compatibility header because some current CS2 HL2SDK checkouts include `tier0/iface.h` but miss the generated protobuf header. The plugin only needs the `ENetworkDisconnectionReason` type for SDK interface declarations; it does not link against protobuf.
