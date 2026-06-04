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
constexpr size_t kExtraPtrOffset = 0x98;

// CMsgTEFireBullets has one generated-protobuf has-bits word before the scalar
// fields. Zeroing scalar values alone can still serialize the fields as
// present, which still allows the client tracer path to run. These bits match
// the field order shown by the CMsgTEFireBullets debug string.
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

bool ReadPointer(const unsigned char *data, size_t size, size_t offset, uintptr_t &value)
{
    if (!data || offset + sizeof(value) > size)
        return false;

    std::memcpy(&value, data + offset, sizeof(value));
    return value != 0;
}

void ClearExtraTypeOnly(unsigned char *data, size_t size)
{
    uintptr_t extraAddress = 0;
    if (!ReadPointer(data, size, kExtraPtrOffset, extraAddress) || extraAddress < 0x10000)
        return;

    auto *extra = reinterpret_cast<unsigned char *>(extraAddress);

    // CMsgTEFireBullets.extra is needed for CS2's normal surface impact path,
    // so do not clear the extra pointer. Only remove the small tracer/type flag
    // inside Extra while leaving attack/render timing and inaccuracy data intact.
    constexpr size_t kCandidateOffsets[] = {0x40, 0x3C, 0x44, 0x38, 0x34};
    for (size_t offset : kCandidateOffsets)
    {
        int32_t value = 0;
        std::memcpy(&value, extra + offset, sizeof(value));
        if (value > 0 && value <= 32)
        {
            value = 0;
            std::memcpy(extra + offset, &value, sizeof(value));
            break;
        }
    }

    // If the Extra object has protobuf has-bits in the small header area, clear
    // field 8's presence bit as well. This is intentionally narrow and will not
    // remove the Extra object or its impact data.
    for (size_t offset = 0x08; offset <= 0x30; offset += sizeof(uint32_t))
    {
        uint32_t bits = 0;
        std::memcpy(&bits, extra + offset, sizeof(bits));
        if ((bits & 0x80u) != 0 && (bits & ~0xFFu) == 0)
        {
            bits &= ~0x80u;
            std::memcpy(extra + offset, &bits, sizeof(bits));
            break;
        }
    }
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

        // Keep the FireBullets message, sound fields, and Extra object alive so
        // normal wall/surface impacts still render. Only neutralize Extra.type,
        // which is the tracer-driving flag for BetterFeedback on suppressed shots.
        ClearExtraTypeOnly(bytes, size);

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
