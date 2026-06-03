#include "silenced_tracer_blocker.h"

/*
    SilencedTracerBlocker

    Purpose:
    - Keep BetterFeedback VPK untouched.
    - Block engine tracer particle dispatch only for silenced weapons.
    - Let BetterFeedback still replace tracers for all non-silenced weapons.

    Integration:
    This file is written as a CS2Fixes-style module. It assumes you are adding it
    into a CS2Fixes fork that already has:
    - gamedata loading
    - detour creation helpers
    - entity/schema helpers

    The exact CS2Fixes helper names change over time, so the bottom section has
    clear adapter points marked ADAPT_TO_YOUR_CS2FIXES_TREE.
*/

#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>

// Forward declarations to avoid pulling every CS2Fixes header into this patch note.
class CBaseEntity;

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

static const std::unordered_set<std::string_view> kSilencedWeaponClassnames = {
    "weapon_usp_silencer",
    "weapon_m4a1_silencer",
    "weapon_mp5sd",
};

static bool IsTracerParticle(const char* particleName)
{
    if (!particleName || !particleName[0])
        return false;

    // BetterFeedback overrides these generic CS2 tracer particle names.
    return std::strstr(particleName, "weapon_tracers") != nullptr;
}

static bool IsSilencedWeaponName(std::string_view name)
{
    return kSilencedWeaponClassnames.find(name) != kSilencedWeaponClassnames.end();
}

// -----------------------------------------------------------------------------
// ADAPT_TO_YOUR_CS2FIXES_TREE
//
// You need to bind these three functions to your CS2Fixes helpers.
// Keep them small. Do not add gameplay logic here.
// -----------------------------------------------------------------------------

static std::string_view GetEntityClassnameSafe(CBaseEntity* entity)
{
    /*
        Replace this with the CS2Fixes helper you use to read entity classnames.

        Common CS2Fixes-style choices:
        - entity->GetClassname()
        - entity->m_pEntity->m_designerName.String()
        - a schema/entity helper already in your tree

        Return empty string if unknown.
    */
    (void)entity;
    return {};
}

static CBaseEntity* GetOwnerPawnOrWeaponOwnerSafe(CBaseEntity* entity)
{
    /*
        Replace this with a helper that returns the player pawn/weapon owner
        when the dispatch entity is a weapon or viewmodel.

        Return nullptr if unknown.
    */
    (void)entity;
    return nullptr;
}

static std::string_view GetActiveWeaponClassnameSafe(CBaseEntity* pawnOrOwner)
{
    /*
        Replace this with schema access to the active weapon classname.

        Typical path:
        CCSPlayerPawn/CBasePlayerPawn
          -> m_pWeaponServices
          -> m_hActiveWeapon
          -> weapon entity classname

        Return empty string if unknown.
    */
    (void)pawnOrOwner;
    return {};
}

// -----------------------------------------------------------------------------
// Silenced weapon detection
// -----------------------------------------------------------------------------

static bool IsSilencedTracerSource(CBaseEntity* dispatchEntity)
{
    if (!dispatchEntity)
        return false;

    // Case 1: particle attached directly to weapon entity.
    const std::string_view directClassname = GetEntityClassnameSafe(dispatchEntity);
    if (IsSilencedWeaponName(directClassname))
        return true;

    // Case 2: particle attached to pawn/viewmodel; inspect active weapon.
    CBaseEntity* ownerOrPawn = GetOwnerPawnOrWeaponOwnerSafe(dispatchEntity);
    if (!ownerOrPawn)
        ownerOrPawn = dispatchEntity;

    const std::string_view weaponClassname = GetActiveWeaponClassnameSafe(ownerOrPawn);
    return IsSilencedWeaponName(weaponClassname);
}

// -----------------------------------------------------------------------------
// Detour
// -----------------------------------------------------------------------------

/*
    DispatchParticleEffect in CS2Fixes gamedata is described as a function with
    9 arguments.

    The exact prototype must match your CS2Fixes branch/gamedata.

    This placeholder keeps the patch readable. Replace DispatchParticleEffectFn
    with the exact typedef already used in your CS2Fixes tree, or create one
    matching your branch's detour code.
*/

using DispatchParticleEffectFn = void (*)(void* recipientFilter,
                                         const char* particleName,
                                         int attachType,
                                         CBaseEntity* entity,
                                         const char* attachmentName,
                                         void* origin,
                                         void* angles,
                                         bool resetAllParticles,
                                         bool unknown);

// ADAPT_TO_YOUR_CS2FIXES_TREE:
// Replace these with CS2Fixes' actual CDetour / DynamicDetour objects.
static DispatchParticleEffectFn g_DispatchParticleEffectOriginal = nullptr;
static void* g_DispatchParticleEffectDetour = nullptr;

static void Detour_DispatchParticleEffect(void* recipientFilter,
                                          const char* particleName,
                                          int attachType,
                                          CBaseEntity* entity,
                                          const char* attachmentName,
                                          void* origin,
                                          void* angles,
                                          bool resetAllParticles,
                                          bool unknown)
{
    if (IsTracerParticle(particleName) && IsSilencedTracerSource(entity))
    {
        // Block only the tracer particle dispatch.
        // Do not block blood, impact_fx, ricochet, taser, etc.
        return;
    }

    if (g_DispatchParticleEffectOriginal)
    {
        g_DispatchParticleEffectOriginal(recipientFilter,
                                         particleName,
                                         attachType,
                                         entity,
                                         attachmentName,
                                         origin,
                                         angles,
                                         resetAllParticles,
                                         unknown);
    }
}

// -----------------------------------------------------------------------------
// Install/remove
// -----------------------------------------------------------------------------

void InstallSilencedTracerBlocker()
{
    /*
        ADAPT_TO_YOUR_CS2FIXES_TREE:

        1. Find function address from gamedata key:
           "DispatchParticleEffect"

        2. Create detour:
           original -> g_DispatchParticleEffectOriginal
           hook     -> Detour_DispatchParticleEffect

        3. Enable detour.

        Keep logs like:
           [SilencedTracerBlocker] Detoured DispatchParticleEffect
    */
}

void RemoveSilencedTracerBlocker()
{
    /*
        ADAPT_TO_YOUR_CS2FIXES_TREE:

        Disable/remove the detour here.
        Restore g_DispatchParticleEffectOriginal to nullptr.
    */
}
