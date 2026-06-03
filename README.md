# SilencedTracerBlocker

A minimal MetaMod:Source plugin for Counter-Strike 2.

This build avoids the old `IGameEventManager2` / `Source2Server` vfunc path that crashed on startup. It hooks CS2's `IGameEventSystem::PostEventAbstract` and blocks only `GE_FireBullets` messages for silenced weapon item definitions:

- `60` = `weapon_m4a1_silencer`
- `61` = `weapon_usp_silencer`
- `23` = `weapon_mp5sd`

It does **not** hook or block:

- `bullet_impact`
- blood events
- wall decal / place decal events
- ricochet events
- taser events
- non-silenced weapon FireBullets messages

## Install

Extract the package contents into your server's `game/csgo` folder.

Expected Windows layout:

```text
game/csgo/addons/metamod/SilencedTracerBlocker.vdf
game/csgo/addons/SilencedTracerBlocker/bin/win64/SilencedTracerBlocker.dll
```

Expected Linux layout:

```text
game/csgo/addons/metamod/SilencedTracerBlocker.vdf
game/csgo/addons/SilencedTracerBlocker/bin/linuxsteamrt64/SilencedTracerBlocker.so
```

Verify:

```text
meta list
```

Debug log:

```text
game/csgo/addons/SilencedTracerBlocker/stb_debug.log
```

## Build on GitHub

Upload this repository to GitHub and run **Actions → Build SilencedTracerBlocker**. Artifacts are named **Linux** and **Windows**.
