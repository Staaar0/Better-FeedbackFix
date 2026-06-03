# CS2Fixes integration notes

This is intentionally small and professional.

## Add source files

Copy:

```text
src/silenced_tracer_blocker.h
src/silenced_tracer_blocker.cpp
```

into your CS2Fixes `src/` folder.

## Add include

In the main CS2Fixes plugin file, add:

```cpp
#include "silenced_tracer_blocker.h"
```

## Add install call

In CS2Fixes plugin load/startup, after gamedata/signatures are ready, add:

```cpp
InstallSilencedTracerBlocker();
```

## Add unload call

In CS2Fixes plugin unload/shutdown, add:

```cpp
RemoveSilencedTracerBlocker();
```

## Add build file entry

Add this source file to your CS2Fixes AMBuild source list:

```text
src/silenced_tracer_blocker.cpp
```

## Add gamedata signature

If your CS2Fixes branch already has `DispatchParticleEffect`, reuse it.

If it does not, copy the entry from:

```text
gamedata/silencedtracerblocker.games.txt
```

## Adapter work required

Open `src/silenced_tracer_blocker.cpp` and replace the three adapter functions with your CS2Fixes helpers:

```cpp
GetEntityClassnameSafe()
GetOwnerPawnOrWeaponOwnerSafe()
GetActiveWeaponClassnameSafe()
```

Those are intentionally isolated so the logic stays clean.

## Test checklist

1. Load server with BetterFeedback VPK unchanged.
2. Use AK/M4A4/Glock/Deagle: custom tracer should still show.
3. Use USP-S/M4A1-S/MP5-SD: tracer should not show.
4. Shoot wall: bullet impact / ricochet should still show.
5. Shoot player: BetterFeedback blood/headshot/taser effects should still show.
