# SilencedTracerBlocker — CS2Fixes-style patch kit

Goal:

```text
Keep BetterFeedback VPK exactly as-is.
Block only built-in tracer particle dispatch for silenced weapons:
- weapon_usp_silencer
- weapon_m4a1_silencer
- weapon_mp5sd

Do not touch:
- BetterFeedback VPK files
- blood_impact
- impact_fx / ricochet / wall impacts
- taser particles
- headshot/helmet particles
```

## Important

This is a **CS2Fixes-style patch kit**, not a standalone Metamod plugin.

Why:
- The correct fix needs a low-level detour for `DispatchParticleEffect`.
- CS2Fixes already has the kind of gamedata/signature/detour infrastructure needed for this.
- A simple CounterStrikeSharp plugin cannot reliably cancel the engine particle dispatch before BetterFeedback replaces the particle.

## Integration plan

1. Fork or clone `Source2ZE/CS2Fixes`.
2. Copy these files into the CS2Fixes source tree:

```text
src/silenced_tracer_blocker.h
src/silenced_tracer_blocker.cpp
```

3. Add the code from `patches/CS2FIXES_INTEGRATION_NOTES.md` into CS2Fixes startup/shutdown code.
4. Add the `DispatchParticleEffect` signature from `gamedata/silencedtracerblocker.games.txt` if your CS2Fixes gamedata does not already have it.
5. Build CS2Fixes normally.

## What the patch does

The detour checks each particle dispatch:

```text
if particle name contains weapon_tracers
and source/current weapon is a silenced weapon
    block/supercede particle dispatch
else
    allow normal dispatch
```

## Silenced weapons blocked

```text
weapon_usp_silencer
weapon_m4a1_silencer
weapon_mp5sd
```

## BetterFeedback result

```text
USP-S / M4A1-S / MP5-SD:
  no BetterFeedback custom tracer

All other weapons:
  BetterFeedback tracer stays unchanged

Wall impacts / blood / taser / headshot / ricochet:
  unchanged
```

## Warning

The function signature for `DispatchParticleEffect` can change after CS2 updates. If the server crashes on load after a CS2 update, disable this patch and update the gamedata/signature.
