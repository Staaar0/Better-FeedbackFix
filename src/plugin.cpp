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

// Current CMsgTEFireBullets generated-protobuf object layout used by CS2.
constexpr size_t kWeaponIdOffset = 0x60;
constexpr size_t kModeOffset = 0x64;
constexpr size_t kSeedOffset = 0x68;
constexpr size_t kPlayerOffset = 0x6C;
constexpr size_t kInaccuracyOffset = 0x70;
constexpr size_t kRecoilIndexOffset = 0x74;
constexpr size_t kSpreadOffset = 0x78;
constexpr size_t kSoundTypeOffset = 0x7C;
constexpr size_t kItemDefOffset = 0x80;
constexpr size_t kSoundDspOffset = 0x84;
constexpr size_t kNumBulletsRemainingOffset = 0x88;
constexpr size_t kAttackTypeOffset = 0x8C;
constexpr size_t kPlayerInAirOffset = 0x90;
constexpr size_t kPlayerScopedOffset = 0x91;
constexpr size_t kTickOffset = 0x94;
constexpr size_t kWeaponIdLiveOffset = 0x98;
constexpr size_t kPlayerLiveOffset = 0x9C;
constexpr uint32_t kDefaultEntityHandle = 0x00FFFFFFu;

constexpr uint32_t kHasBitOrigin = 1u << 0;
constexpr uint32_t kHasBitWeaponId = 1u << 2;
constexpr uint32_t kHasBitSoundType = 1u << 9;
constexpr uint32_t kHasBitItemDef = 1u << 10;
constexpr uint32_t kHasBitSoundDsp = 1u << 11;
constexpr uint32_t kHasBitEntOrigin = 1u << 12;
constexpr uint32_t kAllExpectedFieldBitsMask = 0x00007FFFu;
constexpr uint32_t kKeepSoundFieldBits = kHasBitOrigin | kHasBitSoundType | kHasBitSoundDsp | kHasBitEntOrigin;

void *g_GameEventSystem = nullptr;
void **g_HookedSlot = nullptr;
void *g_OriginalSlotValue = nullptr;
INetworkMessageInternal *g_FireBulletsMsg = nullptr;
INetworkMessageInternal *g_FireBulletsMsgAlt = nullptr;

using PostEventAbstractFn = void (*)(void *, int, bool, void *, INetworkMessageInternal *, const CNetMessage *, unsigned long);
PostEventAbstractFn g_OriginalPostEventAbstract = nullptr;

bool ReadInt32(const unsigned char *data, size_t size, size_t offset, int32_t &value)
{
    if (!data || offset + sizeof(value) > size)
        return false;

    std::memcpy(&value, data + offset, sizeof(value));
    return true;
}

bool WriteInt32(unsigned char *data, size_t size, size_t offset, int32_t value)
{
    if (!data || offset + sizeof(value) > size)
        return false;

    std::memcpy(data + offset, &value, sizeof(value));
    return true;
}

bool ReadUInt32(const unsigned char *data, size_t size, size_t offset, uint32_t &value)
{
    if (!data || offset + sizeof(value) > size)
        return false;

    std::memcpy(&value, data + offset, sizeof(value));
    return true;
}

bool WriteUInt32(unsigned char *data, size_t size, size_t offset, uint32_t value)
{
    if (!data || offset + sizeof(value) > size)
        return false;

    std::memcpy(data + offset, &value, sizeof(value));
    return true;
}

bool ReadFloat(const unsigned char *data, size_t size, size_t offset, float &value)
{
    if (!data || offset + sizeof(value) > size)
        return false;

    std::memcpy(&value, data + offset, sizeof(value));
    return true;
}

bool WriteFloat(unsigned char *data, size_t size, size_t offset, float value)
{
    if (!data || offset + sizeof(value) > size)
        return false;

    std::memcpy(data + offset, &value, sizeof(value));
    return true;
}

bool WriteUInt8(unsigned char *data, size_t size, size_t offset, uint8_t value)
{
    if (!data || offset + sizeof(value) > size)
        return false;

    std::memcpy(data + offset, &value, sizeof(value));
    return true;
}

void HideTracerWeaponIdentity(unsigned char *data, size_t size)
{
    if (!data || size < kPlayerLiveOffset + sizeof(uint32_t))
        return;

    WriteUInt32(data, size, kWeaponIdLiveOffset, kDefaultEntityHandle);
}

bool IsM4A1SOrUSPS(int32_t itemDef)
{
    return itemDef == 60 || itemDef == 61;
}

bool IsMP5SD(int32_t itemDef)
{
    return itemDef == 23;
}

bool IsSuppressedFireBullets(const CNetMessage *message, unsigned long messageSize)
{
    if (!message)
        return false;

    const auto *data = reinterpret_cast<const unsigned char *>(message);
    const size_t size = static_cast<size_t>(messageSize);

    int32_t itemDef = 0;
    if (!ReadInt32(data, size, kItemDefOffset, itemDef))
        return false;

    if (IsMP5SD(itemDef))
        return true;

    if (!IsM4A1SOrUSPS(itemDef))
        return false;

    int32_t soundType = 0;
    if (ReadInt32(data, size, kSoundTypeOffset, soundType) && soundType == 9)
        return true;

    int32_t mode = 0;
    return ReadInt32(data, size, kModeOffset, mode) && mode == 1;
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
    if (IsFireBullets(networkMessage) && IsSuppressedFireBullets(messageData, messageSize))
    {
        auto *bytes = reinterpret_cast<unsigned char *>(const_cast<CNetMessage *>(messageData));
        const size_t size = static_cast<size_t>(messageSize);

        HideTracerWeaponIdentity(bytes, size);

        g_OriginalPostEventAbstract(self, slot, localOnly, filter, networkMessage, messageData, messageSize);
        return;
    }

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

const char *SilencedTracerBlocker::GetAuthor()
{
    return "✪ Stαr";
}

const char *SilencedTracerBlocker::GetName()
{
    return "SilencedTracerBlocker";
}

const char *SilencedTracerBlocker::GetDescription()
{
    return "Blocks BetterFeedback silenced weapon tracers while preserving shot sound and impacts.";
}

const char *SilencedTracerBlocker::GetURL()
{
    return "https://github.com/Staaar0/SilencedTracerBlocker";
}

const char *SilencedTracerBlocker::GetLicense()
{
    return "MIT";
}

const char *SilencedTracerBlocker::GetVersion()
{
    return PLUGIN_FULL_VERSION;
}

const char *SilencedTracerBlocker::GetDate()
{
    return __DATE__;
}

const char *SilencedTracerBlocker::GetLogTag()
{
    return "STB";
}
