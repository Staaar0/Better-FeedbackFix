#include "plugin.h"

#include <networksystem/inetworkmessages.h>
#include <networksystem/netmessage.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <unistd.h>
    #include <sys/mman.h>
    #include <sys/stat.h>
#endif

SilencedTracerBlocker g_SilencedTracerBlocker;
PLUGIN_EXPOSE(SilencedTracerBlocker, g_SilencedTracerBlocker);

namespace
{
// Current CS2 network user-message id for CMsgTEFireBullets. CounterStrikeSharp
// exposes it as CMsgTEFireBullets [452]. Some internal hooks may expose the
// protobuf game-event id instead, so the hook also allows pointer/name matching
// through INetworkMessages when available.
constexpr int kCMsgTEFireBulletsId = 452;
constexpr size_t kPostEventAbstractVtableIndex = 15; // vtable offset 0x78 on x64
constexpr size_t kMaxMessageScanBytes = 0x180;

std::string g_GameDir;
void *g_GameEventSystem = nullptr;
void **g_PostEventSlot = nullptr;
void *g_OriginalPostEventPtr = nullptr;
INetworkMessages *g_NetworkMessages = nullptr;
INetworkMessageInternal *g_FireBulletsMessage = nullptr;
INetworkMessageInternal *g_FireBulletsMessageAlt = nullptr;
std::atomic<int> g_DebugSeenLogs{0};
std::atomic<int> g_DebugBlockLogs{0};

using PostEventAbstractFn = void (*)(void *self,
                                    int slot,
                                    bool localOnly,
                                    void *recipientFilter,
                                    INetworkMessageInternal *networkMessage,
                                    const CNetMessage *messageData,
                                    unsigned long messageSize);

PostEventAbstractFn g_OriginalPostEventAbstract = nullptr;

std::string NormalizeSlashes(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    while (!value.empty() && value.back() == '/')
        value.pop_back();
    return value;
}

std::string GetCurrentWorkingDirectoryString()
{
    char cwd[1024]{};
#ifdef _WIN32
    if (GetCurrentDirectoryA(sizeof(cwd), cwd) > 0)
        return NormalizeSlashes(cwd);
#else
    if (getcwd(cwd, sizeof(cwd)) != nullptr)
        return NormalizeSlashes(cwd);
#endif
    return {};
}

std::string DeriveGameDirFromCwd()
{
    std::string cwd = GetCurrentWorkingDirectoryString();
    const std::string suffixWin = "/game/bin/win64";
    const std::string suffixLinux = "/game/bin/linuxsteamrt64";

    if (cwd.size() >= suffixWin.size() && cwd.compare(cwd.size() - suffixWin.size(), suffixWin.size(), suffixWin) == 0)
        return cwd.substr(0, cwd.size() - suffixWin.size()) + "/game/csgo";

    if (cwd.size() >= suffixLinux.size() && cwd.compare(cwd.size() - suffixLinux.size(), suffixLinux.size(), suffixLinux) == 0)
        return cwd.substr(0, cwd.size() - suffixLinux.size()) + "/game/csgo";

    return cwd;
}

void SetGameDir(ISmmAPI *ismm)
{
    const char *base = ismm ? ismm->GetBaseDir() : nullptr;
    if (base && base[0])
        g_GameDir = NormalizeSlashes(base);
    else
        g_GameDir = DeriveGameDirFromCwd();
}

std::string GamePath(const char *relative)
{
    std::string rel = NormalizeSlashes(relative ? relative : "");
    if (g_GameDir.empty())
        return rel;
    return g_GameDir + "/" + rel;
}

void CreateOneDirectory(const std::string &path)
{
    if (path.empty())
        return;
#ifdef _WIN32
    CreateDirectoryA(path.c_str(), nullptr);
#else
    mkdir(path.c_str(), 0755);
#endif
}

void EnsureDirectoryTree(const char *path)
{
    if (!path || !path[0])
        return;

    std::string current;
    for (const char *p = path; *p; ++p)
    {
        current.push_back(*p);
        if (*p == '/' || *p == '\\')
        {
            if (current.size() > 1)
                CreateOneDirectory(current);
        }
    }
    CreateOneDirectory(current);
}

void AppendLogFile(const char *path, const char *text)
{
    std::ofstream file(path, std::ios::app);
    if (file.is_open())
        file << text;
}

void Log(const char *format, ...)
{
    char buffer[1536];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    META_CONPRINTF("%s", buffer);
    AppendLogFile("stb_debug.log", buffer);

    if (!g_GameDir.empty())
    {
        EnsureDirectoryTree(GamePath("addons/SilencedTracerBlocker").c_str());
        AppendLogFile(GamePath("stb_debug.log").c_str(), buffer);
        AppendLogFile(GamePath("addons/SilencedTracerBlocker/stb_debug.log").c_str(), buffer);
    }
}

void LogCurrentWorkingDirectory()
{
    Log("[STB] Current working directory: %s\n", GetCurrentWorkingDirectoryString().c_str());
    Log("[STB] Game directory: %s\n", g_GameDir.c_str());
}

bool IsSilencedItemDefinition(int32_t itemDef)
{
    return itemDef == 60 ||  // weapon_m4a1_silencer
           itemDef == 61 ||  // weapon_usp_silencer
           itemDef == 23;    // weapon_mp5sd
}

const char *ItemName(int32_t itemDef)
{
    switch (itemDef)
    {
        case 60: return "M4A1-S";
        case 61: return "USP-S";
        case 23: return "MP5-SD";
        default: return "unknown";
    }
}

bool FindSilencedItemDefInFireBullets(const CNetMessage *messageData, int32_t &itemDef, size_t &offset)
{
    if (!messageData)
        return false;

    const auto *bytes = reinterpret_cast<const unsigned char *>(messageData);

    // Generated protobuf objects keep scalar fields in the message object. We
    // scan only a small aligned range and only after the network message has
    // already been identified as CMsgTEFireBullets. This avoids touching blood,
    // impact, ricochet, taser, or normal tracer messages.
    for (size_t i = 0x10; i + sizeof(int32_t) <= kMaxMessageScanBytes; i += sizeof(int32_t))
    {
        int32_t value = 0;
        std::memcpy(&value, bytes + i, sizeof(value));
        if (IsSilencedItemDefinition(value))
        {
            itemDef = value;
            offset = i;
            return true;
        }
    }

    return false;
}

bool IsFireBulletsMessage(INetworkMessageInternal *networkMessage, int &messageId)
{
    messageId = -1;
    if (!networkMessage)
        return false;

    NetMessageInfo_t *info = networkMessage->GetNetMessageInfo();
    if (info)
    {
        messageId = info->m_MessageId;
        if (messageId == kCMsgTEFireBulletsId)
            return true;
    }

    if (networkMessage == g_FireBulletsMessage || networkMessage == g_FireBulletsMessageAlt)
        return true;

    return false;
}

#ifdef _WIN32
bool ProtectPointerSlot(void **slot, DWORD newProtect, DWORD &oldProtect)
{
    return VirtualProtect(slot, sizeof(void *), newProtect, &oldProtect) != 0;
}
#else
uintptr_t PageStart(uintptr_t address)
{
    const auto pageSize = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    return address & ~(pageSize - 1);
}

bool ProtectPointerSlot(void **slot, int protection)
{
    const auto pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void *page = reinterpret_cast<void *>(PageStart(reinterpret_cast<uintptr_t>(slot)));
    return mprotect(page, pageSize, protection) == 0;
}
#endif

bool ReplaceVtableSlot(void *object, size_t index, void *hook, void **original, void ***slotOut)
{
    if (!object || !hook || !original || !slotOut)
        return false;

    void ***objectVtable = reinterpret_cast<void ***>(object);
    void **vtable = *objectVtable;
    if (!vtable)
        return false;

    void **slot = &vtable[index];
    *original = *slot;
    *slotOut = slot;

#ifdef _WIN32
    DWORD oldProtect = 0;
    if (!ProtectPointerSlot(slot, PAGE_EXECUTE_READWRITE, oldProtect))
        return false;
    *slot = hook;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void *), oldProtect, &ignored);
#else
    if (!ProtectPointerSlot(slot, PROT_READ | PROT_WRITE | PROT_EXEC))
        return false;
    *slot = hook;
    ProtectPointerSlot(slot, PROT_READ | PROT_EXEC);
#endif

    return true;
}

void RestoreVtableSlot()
{
    if (!g_PostEventSlot || !g_OriginalPostEventPtr)
        return;

#ifdef _WIN32
    DWORD oldProtect = 0;
    if (ProtectPointerSlot(g_PostEventSlot, PAGE_EXECUTE_READWRITE, oldProtect))
    {
        *g_PostEventSlot = g_OriginalPostEventPtr;
        DWORD ignored = 0;
        VirtualProtect(g_PostEventSlot, sizeof(void *), oldProtect, &ignored);
    }
#else
    if (ProtectPointerSlot(g_PostEventSlot, PROT_READ | PROT_WRITE | PROT_EXEC))
    {
        *g_PostEventSlot = g_OriginalPostEventPtr;
        ProtectPointerSlot(g_PostEventSlot, PROT_READ | PROT_EXEC);
    }
#endif

    g_PostEventSlot = nullptr;
    g_OriginalPostEventPtr = nullptr;
    g_OriginalPostEventAbstract = nullptr;
}

void Hook_PostEventAbstract(void *self,
                            int slot,
                            bool localOnly,
                            void *recipientFilter,
                            INetworkMessageInternal *networkMessage,
                            const CNetMessage *messageData,
                            unsigned long messageSize)
{
    int messageId = -1;
    const bool fireBullets = IsFireBulletsMessage(networkMessage, messageId);

    if (fireBullets)
    {
        int32_t itemDef = 0;
        size_t itemOffset = 0;
        const bool silencedWeapon = FindSilencedItemDefInFireBullets(messageData, itemDef, itemOffset);

        const int seenIndex = g_DebugSeenLogs.fetch_add(1);
        if (seenIndex < 64)
        {
            Log("[STB] CMsgTEFireBullets seen: id=%d data=%p size=%lu silenced_item=%s item_def=%d offset=0x%zx\n",
                messageId,
                static_cast<const void *>(messageData),
                messageSize,
                silencedWeapon ? "yes" : "no",
                itemDef,
                itemOffset);
        }

        if (silencedWeapon)
        {
            const int blockIndex = g_DebugBlockLogs.fetch_add(1);
            if (blockIndex < 64)
            {
                Log("[STB] Blocked silenced weapon FireBullets tracer: item_def=%d weapon=%s offset=0x%zx\n",
                    itemDef,
                    ItemName(itemDef),
                    itemOffset);
            }
            return;
        }
    }

    if (g_OriginalPostEventAbstract)
    {
        g_OriginalPostEventAbstract(self,
                                    slot,
                                    localOnly,
                                    recipientFilter,
                                    networkMessage,
                                    messageData,
                                    messageSize);
    }
}

void *FindInterface(ISmmAPI *ismm, const char *name)
{
    if (!ismm || !name || !name[0])
        return nullptr;

    CreateInterfaceFn factories[] = {
        ismm->GetEngineFactory(),
        ismm->GetServerFactory(),
    };

    for (CreateInterfaceFn factory : factories)
    {
        if (!factory)
            continue;

        void *iface = ismm->VInterfaceMatch(factory, name, 0);
        if (iface)
            return iface;
    }

    return nullptr;
}

} // namespace

bool SilencedTracerBlocker::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
    PLUGIN_SAVEVARS();
    SetGameDir(ismm);

    Log("[STB] Load() entered. version=%s late=%s\n", GetVersion(), late ? "true" : "false");
    LogCurrentWorkingDirectory();

    char hookError[512]{};
    const bool hooked = InstallGameEventSystemHook(ismm, hookError, sizeof(hookError));
    if (!hooked)
    {
        Log("[STB] ERROR: %s\n", hookError[0] ? hookError : "PostEventAbstract hook failed");
        std::snprintf(error, maxlen, "%s", hookError[0] ? hookError : "PostEventAbstract hook failed");
        return false;
    }

    Log("[STB] SilencedTracerBlocker loaded. Hook=GameEventSystem::PostEventAbstract. Impacts/blood/ricochet/taser untouched.\n");
    return true;
}

bool SilencedTracerBlocker::Unload(char *error, size_t maxlen)
{
    RemoveGameEventSystemHook();
    Log("[STB] SilencedTracerBlocker unloaded.\n");
    return true;
}

bool SilencedTracerBlocker::InstallGameEventSystemHook(ISmmAPI *ismm, char *error, size_t maxlen)
{
    g_GameEventSystem = FindInterface(ismm, "GameEventSystemServerV001");
    if (!g_GameEventSystem)
    {
        std::snprintf(error, maxlen, "Could not find GameEventSystemServerV001");
        return false;
    }

#ifdef NETWORKMESSAGES_INTERFACE_VERSION
    g_NetworkMessages = reinterpret_cast<INetworkMessages *>(FindInterface(ismm, NETWORKMESSAGES_INTERFACE_VERSION));
#endif

    if (g_NetworkMessages)
    {
        g_FireBulletsMessage = g_NetworkMessages->FindNetworkMessagePartial("CMsgTEFireBullets");
        g_FireBulletsMessageAlt = g_NetworkMessages->FindNetworkMessagePartial("FireBullets");
        Log("[STB] INetworkMessages found. CMsgTEFireBullets=%p FireBullets=%p\n",
            static_cast<void *>(g_FireBulletsMessage),
            static_cast<void *>(g_FireBulletsMessageAlt));
    }
    else
    {
        Log("[STB] WARNING: INetworkMessages not found. Falling back to message id %d only.\n", kCMsgTEFireBulletsId);
    }

    void *original = nullptr;
    void **slot = nullptr;
    if (!ReplaceVtableSlot(g_GameEventSystem,
                           kPostEventAbstractVtableIndex,
                           reinterpret_cast<void *>(&Hook_PostEventAbstract),
                           &original,
                           &slot))
    {
        std::snprintf(error, maxlen, "Failed to replace GameEventSystemServerV001 vtable slot %zu", kPostEventAbstractVtableIndex);
        return false;
    }

    g_PostEventSlot = slot;
    g_OriginalPostEventPtr = original;
    g_OriginalPostEventAbstract = reinterpret_cast<PostEventAbstractFn>(original);

    Log("[STB] Hooked GameEventSystemServerV001 vtable[%zu]. original=%p hook=%p\n",
        kPostEventAbstractVtableIndex,
        original,
        reinterpret_cast<void *>(&Hook_PostEventAbstract));
    return true;
}

void SilencedTracerBlocker::RemoveGameEventSystemHook()
{
    RestoreVtableSlot();
    g_GameEventSystem = nullptr;
    g_NetworkMessages = nullptr;
    g_FireBulletsMessage = nullptr;
    g_FireBulletsMessageAlt = nullptr;
}
