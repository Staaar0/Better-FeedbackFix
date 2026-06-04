# SilencedTracerBlocker

MetaMod:Source plugin for CS2.

Made for the Better FeedBack workshop addon. It hides custom tracers for suppressed M4A1-S / USP-S shots while keeping the normal shot sound and leaving other effects untouched.

## Behavior

- M4A1-S / USP-S with suppressor attached: tracer hidden, sound kept
- M4A1-S / USP-S with suppressor removed: tracer allowed, sound kept
- MP5-SD: tracer hidden, sound kept
- Non-silenced weapons: untouched
- Blood, wall impacts, decals, ricochet, taser: untouched

## Install

Copy the packaged `addons` folder into `game/csgo`.


## SS-22

Preserves the FireBullets `extra` object for wall/surface impacts and clears only the suppressed-shot tracer type flag.
