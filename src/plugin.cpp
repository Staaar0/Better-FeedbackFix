#include "plugin.h"

#include <networksystem/inetworkmessages.h>
#include <networksystem/netmessage.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <sys/mman.h>
    #include <unistd.h>
#endif

SilencedTracerBlocker g_SilencedTracerBlocker;
PLUGIN_EXPOSE(SilencedTracerBlocker, g_SilencedTracerBlocker);

namespace
{
constexpr int kFireBulletsId = 452;
constexpr size_t kPostEventIndex = 15;
constexpr size_t kItemDefOffset = 0x80;
constexpr size_t kFallbackScanBytes = 0x180;

void *g_GameEventSystem = nullptr;
void **g_HookedSlot = nullptr;
void *g_OriginalSlotValue = nullptr;
INetworkMessageInternal *g_FireBulletsMsg = nullptr;
INetworkMessageInternal *g_FireBulletsMsgAlt = nullptr;

using PostEventAbstractFn = void (*)(void *, int, bool, void *, INetworkMessageInternal *, const CNetMessage *, unsigned long);
PostEventAbstractFn g_OriginalPostEventAbstract = nullptr;

bool IsSilencedWeapon(int32_t itemDef)
{
    return itemDef == 60 || itemDef == 61 || itemDef == 23; // M4A1-S, USP-S, MP5-SD
}

bool ReadInt32(const unsigned char *data, size_t size, size_t offset, int32_t &value)
{
    if (!data || offset + sizeof(value) > size)
        return false;

    std::memcpy(&value, data + offset, sizeof(value));
    return true;
}

bool IsSilencedFireBullets(const CNetMessage *message, unsigned long messageSize)
{
    if (!message)
        return false;

    const auto *data = reinterpret_cast<const unsigned char *>(message);
    const size_t size = static_cast<size_t>(messageSize);

    int32_t itemDef = 0;
    if (ReadInt32(data, size, kItemDefOffset, itemDef) && IsSilencedWeapon(itemDef))
        return true;

    // Small compatibility fallback for layout changes. Runs only on CMsgTEFireBullets.
    const size_t maxScan = (size < kFallbackScanBytes) ? size : kFallbackScanBytes;
    for (size_t offset = 0x10; offset + sizeof(itemDef) <= maxScan; offset += sizeof(itemDef))
    {
        if (offset == kItemDefOffset)
            continue;

        if (ReadInt32(data, maxScan, offset, itemDef) && IsSilencedWeapon(itemDef))
            return true;
    }

    return false;
}

bool IsFireBullets(INetworkMessageInternal *networkMessage)
{
    if (!networkMessage)
        return false;

    NetMessageInfo_t *info = networkMessage->GetNetMessageInfo();
    return (info && info->m_MessageId == kFireBulletsId) ||
           networkMessage == g_FireBulletsMsg ||
           networkMessage == g_FireBulletsMsgAlt;
}

void Hook_PostEventAbstract(void *self, int slot, bool localOnly, void *filter,
                            INetworkMessageInternal *networkMessage,
                            const CNetMessage *messageData,
                            unsigned long messageSize)
{
    if (IsFireBullets(networkMessage) && IsSilencedFireBullets(messageData, messageSize))
        return;

    g_OriginalPostEventAbstract(self, slot, localOnly, filter, networkMessage, messageData, messageSize);
}

void *FindInterface(ISmmAPI *ismm, const char *name)
{
    CreateInterfaceFn factories[] = {ismm->GetEngineFactory(), ismm->GetServerFactory()};
    for (CreateInterfaceFn factory : factories)
    {
        if (factory)
        {
            void *iface = ismm->VInterfaceMatch(factory, name, 0);
            if (iface)
                return iface;
        }
    }
    return nullptr;
}

#ifdef _WIN32
bool ProtectSlot(void **slot, DWORD protection, DWORD &oldProtection)
{
    return VirtualProtect(slot, sizeof(void *), protection, &oldProtection) != 0;
}
#else
uintptr_t PageStart(uintptr_t address)
{
    const auto pageSize = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    return address & ~(pageSize - 1);
}

bool ProtectSlot(void **slot, int protection)
{
    const auto pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void *page = reinterpret_cast<void *>(PageStart(reinterpret_cast<uintptr_t>(slot)));
    return mprotect(page, pageSize, protection) == 0;
}
#endif

bool HookVtableSlot(void *object, size_t index, void *hook)
{
    if (!object || !hook)
        return false;

    void **slot = &(*reinterpret_cast<void ***>(object))[index];
    g_OriginalSlotValue = *slot;
    g_OriginalPostEventAbstract = reinterpret_cast<PostEventAbstractFn>(g_OriginalSlotValue);
    g_HookedSlot = slot;

#ifdef _WIN32
    DWORD oldProtection = 0;
    if (!ProtectSlot(slot, PAGE_EXECUTE_READWRITE, oldProtection))
        return false;
    *slot = hook;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void *), oldProtection, &ignored);
#else
    if (!ProtectSlot(slot, PROT_READ | PROT_WRITE | PROT_EXEC))
        return false;
    *slot = hook;
    ProtectSlot(slot, PROT_READ | PROT_EXEC);
#endif

    return true;
}

void UnhookVtableSlot()
{
    if (!g_HookedSlot || !g_OriginalSlotValue)
        return;

#ifdef _WIN32
    DWORD oldProtection = 0;
    if (ProtectSlot(g_HookedSlot, PAGE_EXECUTE_READWRITE, oldProtection))
    {
        *g_HookedSlot = g_OriginalSlotValue;
        DWORD ignored = 0;
        VirtualProtect(g_HookedSlot, sizeof(void *), oldProtection, &ignored);
    }
#else
    if (ProtectSlot(g_HookedSlot, PROT_READ | PROT_WRITE | PROT_EXEC))
    {
        *g_HookedSlot = g_OriginalSlotValue;
        ProtectSlot(g_HookedSlot, PROT_READ | PROT_EXEC);
    }
#endif

    g_HookedSlot = nullptr;
    g_OriginalSlotValue = nullptr;
    g_OriginalPostEventAbstract = nullptr;
}
} // namespace

bool SilencedTracerBlocker::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
    PLUGIN_SAVEVARS();

    g_GameEventSystem = FindInterface(ismm, "GameEventSystemServerV001");
    if (!g_GameEventSystem)
    {
        std::snprintf(error, maxlen, "Could not find GameEventSystemServerV001");
        return false;
    }

#ifdef NETWORKMESSAGES_INTERFACE_VERSION
    auto *networkMessages = reinterpret_cast<INetworkMessages *>(FindInterface(ismm, NETWORKMESSAGES_INTERFACE_VERSION));
    if (networkMessages)
    {
        g_FireBulletsMsg = networkMessages->FindNetworkMessagePartial("CMsgTEFireBullets");
        g_FireBulletsMsgAlt = networkMessages->FindNetworkMessagePartial("FireBullets");
    }
#endif

    if (!HookVtableSlot(g_GameEventSystem, kPostEventIndex, reinterpret_cast<void *>(&Hook_PostEventAbstract)))
    {
        std::snprintf(error, maxlen, "Failed to hook GameEventSystemServerV001::PostEventAbstract");
        return false;
    }

    return true;
}

bool SilencedTracerBlocker::Unload(char *error, size_t maxlen)
{
    UnhookVtableSlot();
    g_GameEventSystem = nullptr;
    g_FireBulletsMsg = nullptr;
    g_FireBulletsMsgAlt = nullptr;
    return true;
}
