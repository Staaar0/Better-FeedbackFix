# SilencedTracerBlocker

MetaMod:Source plugin for CS2 that blocks tracer particle dispatch for silenced weapons only.

Goal:

- USP-S / M4A1-S / MP5-SD: suppress tracer particles so BetterFeedback does not turn them into visible custom tracers.
- AK/M4A4/AWP/other non-silenced weapons: untouched, so BetterFeedback custom tracers still work.
- Wall bullet impacts, blood impacts, decals, ricochet, taser effects: untouched.

## Important runtime notes

This version includes two live-server fixes:

1. It no longer depends only on `weapon_fire.silenced` because that bool can be inconsistent on live CS2 servers. It also treats these weapon names as silenced-tracer candidates:
   - `weapon_usp_silencer`
   - `weapon_m4a1_silencer`
   - `weapon_mp5sd`

2. It matches tracer particle names more broadly by checking for `tracer`, while explicitly excluding:
   - `impact`
   - `blood`
   - `decal`
   - `ricochet`
   - `taser`

When testing, the server console should print up to 16 debug lines like:

```txt
[STB] Silenced weapon_fire detected: weapon=weapon_m4a1_silencer silenced=false
[STB] Blocked silenced tracer particle: particles/...
```

If you only see the first line and never see `Blocked silenced tracer particle`, then the current detour signature is not the function used by weapon tracers on your server build, and the next step is to hook the CS2 `CUserMsg_ParticleManager` / `ParticleTracer` send path instead.

## Build in GitHub

1. Upload this repository to GitHub.
2. Open **Actions**.
3. Run **Build SilencedTracerBlocker**.
4. Download the Linux and Windows artifacts.

## Install

Copy the packaged `addons/SilencedTracerBlocker` folder and `addons/metamod/SilencedTracerBlocker.vdf` into your CS2 server.

## Gamedata

Current included signatures:

- Linux: from your uploaded Linux gamedata.
- Windows: extracted from your uploaded `server.dll`.

If CS2 updates and the plugin stops loading, update `addons/SilencedTracerBlocker/gamedata/silenced_tracer_blocker.games.txt`.


## SS-5 diagnostics

This build adds a SourceHook hook on `IGameEventManager2::FireEvent` in addition to the normal event listener. It also writes diagnostics to:

```text
addons/SilencedTracerBlocker/stb_debug.log
```

When testing M4A1-S or USP-S you should see:

```text
[STB] SilencedTracerBlocker loaded... FireEventHook=on
[STB] weapon_fire seen via fireevent-hook: weapon=weapon_m4a1_silencer ...
[STB] Silenced weapon_fire detected via fireevent-hook: ...
[STB] Tracer particle observed: ...
[STB] Blocked silenced tracer particle: ...
```

If you see `weapon_fire` lines but no `Tracer particle observed` lines, then CS2/BetterFeedback is not using the hooked `DispatchParticleEffect` path for the visible tracer and the next hook must target the particle user-message send path instead.


## SS-6 load diagnostic build

This build proves whether MetaMod is actually loading the plugin. It writes a load marker as soon as `Load()` starts, before gamedata and before the particle detour.

Search these locations after server start:

```text
stb_debug.log
addons/SilencedTracerBlocker/stb_debug.log
csgo/addons/SilencedTracerBlocker/stb_debug.log
game/csgo/addons/SilencedTracerBlocker/stb_debug.log
```

Expected first lines:

```text
[STB] Load() entered...
[STB] Current working directory: ...
[STB] SilencedTracerBlocker loaded... GameData=... ParticleDetour=...
```

If none of these files exists and the console does not print `[STB] Load() entered`, MetaMod did not load the plugin VDF/binary. Check `meta list` and make sure the package contents are copied into the CS2 `game/csgo` folder, not into the server root or an extra nested `cs2` folder.
