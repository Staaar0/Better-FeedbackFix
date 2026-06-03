# SilencedTracerBlocker

MetaMod:Source plugin for CS2 servers using the Better Feedback workshop addon.

It hides Better Feedback tracers only for suppressed silencer weapons while keeping shot sounds, wall impacts, blood impacts, decals, ricochet, taser, and normal weapon tracers untouched.

## Behavior

- M4A1-S / USP-S with suppressor attached: tracer hidden, sound kept.
- M4A1-S / USP-S with suppressor removed: tracer allowed.
- MP5-SD: tracer hidden.
- Non-silenced weapons: untouched.

## Install

Copy the packaged `addons` folder into `game/csgo`.
