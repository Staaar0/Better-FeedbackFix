#pragma once

// Minimal compile shim for HL2SDK-CS2 eiface.h.
// SilencedTracerBlocker does not use NetworkDisconnectionReason values.
// This only satisfies the type used in interface method signatures.

enum ENetworkDisconnectionReason : int
{
    NETWORK_DISCONNECT_INVALID = 0
};
