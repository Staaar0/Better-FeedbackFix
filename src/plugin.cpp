#include "plugin.h"

#include <sourcehook.h>
#include <eiface.h>
#include <te.pb.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>

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
    #include <sys/stat.h>
#endif

SilencedTracerBlocker g_SilencedTracerBlocker;
PLUGIN_EXPOSE(SilencedTracerBlocker, g_SilencedTracerBlocker);

static IGameEventSystem *g_GameEventSystem = nullptr;
static bool g_PostEventHooked = false;
static std::string g_GameDir;
static int g_DebugSeenFireBullets = 0;
static int g_DebugBlockedFireBullets = 0;

SH_DECL_HOOK8_void(IGameEventSystem,
                   PostEventAbstract,
                   SH_NOATTRIB,
                   0,
                   CSplitScreenSlot,
                   bool,
                   int,
                   const uint64 *,
                   INetworkMessageInternal *,
                   const CNetMessage *,
                   unsigned long,
                   NetChannelBufType_t);

namespace
{
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
        const char c = *p;
        current.push_back(c);
        if (c == '/' || c == '\\')
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
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    META_CONPRINTF("%s", buffer);
    AppendLogFile("stb_debug.log", buffer);

    if (!g_GameDir.empty())
    {
        const std::string addonDir = GamePath("addons/SilencedTracerBlocker");
        EnsureDirectoryTree(addonDir.c_str());
        AppendLogFile(GamePath("stb_debug.log").c_str(), buffer);
        AppendLogFile(GamePath("addons/SilencedTracerBlocker/stb_debug.log").c_str(), buffer);
    }
}

bool IsSilencedItemDefinition(int32_t itemDefIndex)
{
    // CS2 item definition indexes:
    // 60 = weapon_m4a1_silencer, 61 = weapon_usp_silencer, 23 = weapon_mp5sd.
    return itemDefIndex == 60 || itemDefIndex == 61 || itemDefIndex == 23;
}

const char *SilencedWeaponName(int32_t itemDefIndex)
{
    switch (itemDefIndex)
    {
        case 60: return "weapon_m4a1_silencer";
        case 61: return "weapon_usp_silencer";
        case 23: return "weapon_mp5sd";
        default: return "unknown";
    }
}

IGameEventSystem *AcquireGameEventSystem(ISmmAPI *ismm)
{
    if (!ismm || !ismm->GetEngineFactory())
        return nullptr;

    auto *eventSystem = reinterpret_cast<IGameEventSystem *>(
        ismm->VInterfaceMatch(ismm->GetEngineFactory(), GAMEEVENTSYSTEM_INTERFACE_VERSION, 0));

    return eventSystem;
}
} // namespace

bool SilencedTracerBlocker::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
    PLUGIN_SAVEVARS();
    SetGameDir(ismm);

    Log("[STB] Load() entered. version=%s late=%s\n", GetVersion(), late ? "true" : "false");
    Log("[STB] Current working directory: %s\n", GetCurrentWorkingDirectoryString().c_str());
    Log("[STB] Game directory: %s\n", g_GameDir.c_str());

    g_GameEventSystem = AcquireGameEventSystem(ismm);
    if (!g_GameEventSystem)
    {
        Log("[STB] ERROR: Could not find %s. Plugin will load safely but cannot block tracers.\n",
            GAMEEVENTSYSTEM_INTERFACE_VERSION);
        return true;
    }

    SH_ADD_HOOK(IGameEventSystem,
                PostEventAbstract,
                g_GameEventSystem,
                SH_MEMBER(this, &SilencedTracerBlocker::Hook_PostEvent),
                false);
    g_PostEventHooked = true;

    Log("[STB] Hooked IGameEventSystem::PostEventAbstract. Silenced FireBullets messages will be blocked; impacts/blood/decal events are untouched.\n");
    return true;
}

bool SilencedTracerBlocker::Unload(char *error, size_t maxlen)
{
    if (g_GameEventSystem && g_PostEventHooked)
    {
        SH_REMOVE_HOOK(IGameEventSystem,
                       PostEventAbstract,
                       g_GameEventSystem,
                       SH_MEMBER(this, &SilencedTracerBlocker::Hook_PostEvent),
                       false);
        g_PostEventHooked = false;
    }

    Log("[STB] SilencedTracerBlocker unloaded.\n");
    return true;
}

void SilencedTracerBlocker::Hook_PostEvent(CSplitScreenSlot nSlot,
                                           bool bLocalOnly,
                                           int nClientCount,
                                           const uint64 *clients,
                                           INetworkMessageInternal *pEvent,
                                           const CNetMessage *pData,
                                           unsigned long nSize,
                                           NetChannelBufType_t bufType)
{
    if (!pEvent || !pData)
        RETURN_META(MRES_IGNORED);

    NetMessageInfo_t *info = pEvent->GetNetMessageInfo();
    if (!info || info->m_MessageId != GE_FireBulletsId)
        RETURN_META(MRES_IGNORED);

    auto *msg = const_cast<CNetMessage *>(pData)->ToPB<CMsgTEFireBullets>();
    if (!msg)
        RETURN_META(MRES_IGNORED);

    const int32_t itemDefIndex = msg->item_def_index();
    const int seen = g_DebugSeenFireBullets++;
    if (seen < 32)
    {
        Log("[STB] GE_FireBullets seen: item_def_index=%d weapon_id=%d sound_type=%d mode=%d clients=%d\n",
            itemDefIndex,
            msg->weapon_id(),
            msg->sound_type(),
            msg->mode(),
            nClientCount);
    }

    if (IsSilencedItemDefinition(itemDefIndex))
    {
        const int blocked = g_DebugBlockedFireBullets++;
        if (blocked < 64)
        {
            Log("[STB] Blocked silenced GE_FireBullets tracer source: %s item_def_index=%d\n",
                SilencedWeaponName(itemDefIndex),
                itemDefIndex);
        }

        RETURN_META(MRES_SUPERCEDE);
    }

    RETURN_META(MRES_IGNORED);
}
