#pragma once

// CS2Fixes-style patch module.
// Add InstallSilencedTracerBlocker() on plugin load.
// Add RemoveSilencedTracerBlocker() on plugin unload.

void InstallSilencedTracerBlocker();
void RemoveSilencedTracerBlocker();
