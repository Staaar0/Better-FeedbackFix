# SilencedTracerBlocker

Standalone Metamod:Source plugin repo for CS2.

Goal:

```text
Keep BetterFeedback VPK unchanged.

Block only weapon tracer particles when a silenced weapon fired:
- weapon_usp_silencer
- weapon_m4a1_silencer
- weapon_mp5sd

Everything else stays untouched:
- BetterFeedback blood
- wall impact / ricochet
- taser
- helmet/headshot impact
- non-silencer BetterFeedback tracers
```

## Important honesty note

This repo is **standalone from CS2Fixes**. You do not need to fork CS2Fixes.

But every Metamod plugin still needs:
- Metamod:Source headers
- HL2SDK-CS2 headers

The GitHub workflow downloads those automatically.

## How it works

1. Listen to `weapon_fire`.
2. If the fired weapon is silenced, open a very small tracer-block window.
3. Detour `DispatchParticleEffect`.
4. If the particle name contains `weapon_tracers` during that silenced window, block only that tracer particle.
5. All other particles are allowed.

This keeps BetterFeedback installed as-is, but prevents the silenced weapon tracer dispatch from reaching clients.

## Build on GitHub

Upload this repo to GitHub, then:

```text
Actions
→ Build SilencedTracerBlocker
→ Run workflow
```

Download the artifact named:

```text
SilencedTracerBlocker-package
```

## Install

Copy the artifact files to your server so it becomes:

```text
game/csgo/addons/metamod/silencedtracerblocker.vdf
game/csgo/addons/silencedtracerblocker/bin/linuxsteamrt64/silencedtracerblocker_mm.so
game/csgo/addons/silencedtracerblocker/gamedata/silenced_tracer_blocker.games.txt
```

Then restart server and run:

```text
meta list
```

## Notes

This first standalone version is Linux-only. Most CS2 dedicated servers use Linux.

If CS2 updates and the plugin stops loading/crashes, update:

```text
gamedata/silenced_tracer_blocker.games.txt
```

Specifically the `DispatchParticleEffect` signature.
