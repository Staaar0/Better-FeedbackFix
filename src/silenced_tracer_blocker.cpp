#include <ISmmPlugin.h>
#include <igameevents.h>

#include <atomic>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <sys/mman.h>
#include <link.h>

#ifndef META_IS_SOURCE2
#define META_IS_SOURCE2 1
#endif

class SilencedTracerBlocker final : public ISmmPlugin, public IGameEventListener2
{
public:
    bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late) override;
    bool Unload(char* error, size_t maxlen) override;
    void FireGameEvent(IGameEvent* event) override;
    int GetEventDebugID() override { return EVENT_DEBUG_ID_INIT; }

    const char* GetAuthor() override { return "Staar0 + ChatGPT"; }
    const char* GetName() override { return "SilencedTracerBlocker"; }
    const char* GetDescription() override { return "Blocks weapon_tracers particle dispatch for silenced weapons only."; }
    const char* GetURL() override { return ""; }
    const char* GetLicense() override { return "MIT"; }
    const char* GetVersion() override { return "0.1.0"; }
    const char* GetDate() override { return __DATE__; }
    const char* GetLogTag() override { return "STB"; }

private:
    bool LoadGameData(char* error, size_t maxlen);
    bool InstallDispatchDetour(char* error, size_t maxlen);
    void RemoveDispatchDetour();

    bool ShouldBlockTracerNow() const;
};

SilencedTracerBlocker g_SilencedTracerBlocker;
PLUGIN_EXPOSE(SilencedTracerBlocker, g_SilencedTracerBlocker);

ISmmAPI* g_SMAPI = nullptr;
PluginId g_PLID = 0;
IGameEventManager2* g_GameEvents = nullptr;

static std::atomic<long long> g_BlockUntilMs{0};

static std::string g_DispatchPatternText;
static std::vector<int> g_DispatchPattern;

// Linux x86_64 trampoline detour.
// This is intentionally small. It uses a 12-byte absolute jump:
// mov rax, imm64
// jmp rax
using DispatchParticleEffectFn = int (*)(const char* particleName,
                                         int attachType,
                                         void* entity,
                                         char attachmentPoint,
                                         void* attachmentName,
                                         bool resetAllParticlesOnEntity,
                                         int splitScreenPlayerSlot,
                                         void* recipientFilter,
                                         unsigned char* unknown);

static DispatchParticleEffectFn g_OriginalDispatchParticleEffect = nullptr;
static void* g_DispatchParticleAddress = nullptr;
static void* g_Trampoline = nullptr;
static unsigned char g_OriginalBytes[16]{};
static constexpr size_t kPatchSize = 16;

static long long NowMs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000LL;
}

static bool IsSilencedWeapon(std::string_view weapon)
{
    if (weapon == "weapon_usp_silencer" || weapon == "usp_silencer")
        return true;
    if (weapon == "weapon_m4a1_silencer" || weapon == "m4a1_silencer")
        return true;
    if (weapon == "weapon_mp5sd" || weapon == "mp5sd")
        return true;
    return false;
}

static bool IsTracerParticle(const char* particleName)
{
    return particleName && std::strstr(particleName, "weapon_tracers") != nullptr;
}

static int Detour_DispatchParticleEffect(const char* particleName,
                                         int attachType,
                                         void* entity,
                                         char attachmentPoint,
                                         void* attachmentName,
                                         bool resetAllParticlesOnEntity,
                                         int splitScreenPlayerSlot,
                                         void* recipientFilter,
                                         unsigned char* unknown)
{
    if (IsTracerParticle(particleName) && g_SilencedTracerBlocker.ShouldBlockTracerNow())
    {
        // Only block the tracer particle.
        // Do not block blood, wall impacts, ricochet, taser, headshot, etc.
        return 0;
    }

    return g_OriginalDispatchParticleEffect
        ? g_OriginalDispatchParticleEffect(particleName,
                                           attachType,
                                           entity,
                                           attachmentPoint,
                                           attachmentName,
                                           resetAllParticlesOnEntity,
                                           splitScreenPlayerSlot,
                                           recipientFilter,
                                           unknown)
        : 0;
}

static std::vector<int> ParsePattern(const std::string& text)
{
    std::vector<int> out;
    std::istringstream iss(text);
    std::string token;

    while (iss >> token)
    {
        if (token == "?" || token == "??")
        {
            out.push_back(-1);
            continue;
        }

        out.push_back(static_cast<int>(std::strtol(token.c_str(), nullptr, 16)));
    }

    return out;
}

static bool FindModule(const char* namePart, uintptr_t& base, size_t& size)
{
    std::ifstream maps("/proc/self/maps");
    std::string line;
    uintptr_t minAddr = UINTPTR_MAX;
    uintptr_t maxAddr = 0;

    while (std::getline(maps, line))
    {
        if (line.find(namePart) == std::string::npos)
            continue;

        uintptr_t start = 0, end = 0;
        if (std::sscanf(line.c_str(), "%lx-%lx", &start, &end) != 2)
            continue;

        if (start < minAddr) minAddr = start;
        if (end > maxAddr) maxAddr = end;
    }

    if (minAddr == UINTPTR_MAX || maxAddr <= minAddr)
        return false;

    base = minAddr;
    size = maxAddr - minAddr;
    return true;
}

static void* PatternScan(uintptr_t base, size_t size, const std::vector<int>& pattern)
{
    if (pattern.empty() || size < pattern.size())
        return nullptr;

    auto* data = reinterpret_cast<unsigned char*>(base);

    for (size_t i = 0; i <= size - pattern.size(); ++i)
    {
        bool match = true;

        for (size_t j = 0; j < pattern.size(); ++j)
        {
            if (pattern[j] != -1 && data[i + j] != static_cast<unsigned char>(pattern[j]))
            {
                match = false;
                break;
            }
        }

        if (match)
            return data + i;
    }

    return nullptr;
}

static bool WriteJump(void* from, void* to)
{
    auto pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    auto page = reinterpret_cast<uintptr_t>(from) & ~(pageSize - 1);

    if (mprotect(reinterpret_cast<void*>(page), pageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return false;

    unsigned char patch[kPatchSize]{};
    patch[0] = 0x48; // mov rax, imm64
    patch[1] = 0xB8;
    *reinterpret_cast<uint64_t*>(&patch[2]) = reinterpret_cast<uint64_t>(to);
    patch[10] = 0xFF; // jmp rax
    patch[11] = 0xE0;
    for (size_t i = 12; i < kPatchSize; ++i)
        patch[i] = 0x90;

    std::memcpy(from, patch, kPatchSize);
    __builtin___clear_cache(reinterpret_cast<char*>(from), reinterpret_cast<char*>(from) + kPatchSize);

    mprotect(reinterpret_cast<void*>(page), pageSize, PROT_READ | PROT_EXEC);
    return true;
}

bool SilencedTracerBlocker::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
    PLUGIN_SAVEVARS();

    g_SMAPI = ismm;
    g_PLID = id;

    if (!LoadGameData(error, maxlen))
        return false;

    // Try to get game event manager.
    // Different MM:S builds can expose factories differently; this is the normal path.
    if (g_SMAPI && g_SMAPI->GetEngineFactory(false))
    {
        g_GameEvents = static_cast<IGameEventManager2*>(
            g_SMAPI->GetEngineFactory(false)(INTERFACEVERSION_GAMEEVENTSMANAGER2, nullptr)
        );
    }

    if (g_GameEvents)
        g_GameEvents->AddListener(this, "weapon_fire", true);

    if (!InstallDispatchDetour(error, maxlen))
        return false;

    META_CONPRINTF("[SilencedTracerBlocker] Loaded\n");
    return true;
}

bool SilencedTracerBlocker::Unload(char* error, size_t maxlen)
{
    if (g_GameEvents)
        g_GameEvents->RemoveListener(this);

    RemoveDispatchDetour();

    META_CONPRINTF("[SilencedTracerBlocker] Unloaded\n");
    return true;
}

void SilencedTracerBlocker::FireGameEvent(IGameEvent* event)
{
    if (!event)
        return;

    const char* name = event->GetName();
    if (!name || std::strcmp(name, "weapon_fire") != 0)
        return;

    const char* weapon = event->GetString("weapon", "");
    if (!IsSilencedWeapon(weapon))
        return;

    // Block tracer dispatches caused by this fire event for a tiny window.
    // This keeps the plugin simple and avoids touching BetterFeedback VPK files.
    g_BlockUntilMs.store(NowMs() + 80);
}

bool SilencedTracerBlocker::ShouldBlockTracerNow() const
{
    return NowMs() <= g_BlockUntilMs.load();
}

bool SilencedTracerBlocker::LoadGameData(char* error, size_t maxlen)
{
    // Simple text gamedata reader.
    // Expected line:
    // DispatchParticleEffect = 55 48 89 E5 ...
    std::ifstream file("addons/silencedtracerblocker/gamedata/silenced_tracer_blocker.games.txt");
    if (!file.is_open())
    {
        std::snprintf(error, maxlen, "Could not open gamedata/silenced_tracer_blocker.games.txt");
        return false;
    }

    std::string line;
    while (std::getline(file, line))
    {
        auto comment = line.find("//");
        if (comment != std::string::npos)
            line.erase(comment);

        auto pos = line.find("DispatchParticleEffect");
        if (pos == std::string::npos)
            continue;

        auto eq = line.find("=");
        if (eq == std::string::npos)
            continue;

        g_DispatchPatternText = line.substr(eq + 1);
        g_DispatchPattern = ParsePattern(g_DispatchPatternText);
        break;
    }

    if (g_DispatchPattern.empty())
    {
        std::snprintf(error, maxlen, "DispatchParticleEffect pattern missing/empty");
        return false;
    }

    return true;
}

bool SilencedTracerBlocker::InstallDispatchDetour(char* error, size_t maxlen)
{
    uintptr_t serverBase = 0;
    size_t serverSize = 0;

    if (!FindModule("server.so", serverBase, serverSize))
    {
        std::snprintf(error, maxlen, "Could not find server.so in /proc/self/maps");
        return false;
    }

    g_DispatchParticleAddress = PatternScan(serverBase, serverSize, g_DispatchPattern);
    if (!g_DispatchParticleAddress)
    {
        std::snprintf(error, maxlen, "DispatchParticleEffect signature not found");
        return false;
    }

    std::memcpy(g_OriginalBytes, g_DispatchParticleAddress, kPatchSize);

    // Allocate trampoline.
    const size_t trampolineSize = kPatchSize + 16;
    g_Trampoline = mmap(nullptr,
                        trampolineSize,
                        PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS,
                        -1,
                        0);

    if (g_Trampoline == MAP_FAILED)
    {
        g_Trampoline = nullptr;
        std::snprintf(error, maxlen, "mmap trampoline failed");
        return false;
    }

    std::memcpy(g_Trampoline, g_OriginalBytes, kPatchSize);

    void* returnAddress = reinterpret_cast<unsigned char*>(g_DispatchParticleAddress) + kPatchSize;
    WriteJump(reinterpret_cast<unsigned char*>(g_Trampoline) + kPatchSize, returnAddress);

    g_OriginalDispatchParticleEffect = reinterpret_cast<DispatchParticleEffectFn>(g_Trampoline);

    if (!WriteJump(g_DispatchParticleAddress, reinterpret_cast<void*>(&Detour_DispatchParticleEffect)))
    {
        std::snprintf(error, maxlen, "Failed to write dispatch detour");
        return false;
    }

    META_CONPRINTF("[SilencedTracerBlocker] DispatchParticleEffect detoured\n");
    return true;
}

void SilencedTracerBlocker::RemoveDispatchDetour()
{
    if (!g_DispatchParticleAddress)
        return;

    auto pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    auto page = reinterpret_cast<uintptr_t>(g_DispatchParticleAddress) & ~(pageSize - 1);

    if (mprotect(reinterpret_cast<void*>(page), pageSize, PROT_READ | PROT_WRITE | PROT_EXEC) == 0)
    {
        std::memcpy(g_DispatchParticleAddress, g_OriginalBytes, kPatchSize);
        __builtin___clear_cache(reinterpret_cast<char*>(g_DispatchParticleAddress),
                                reinterpret_cast<char*>(g_DispatchParticleAddress) + kPatchSize);
        mprotect(reinterpret_cast<void*>(page), pageSize, PROT_READ | PROT_EXEC);
    }

    if (g_Trampoline)
    {
        munmap(g_Trampoline, kPatchSize + 16);
        g_Trampoline = nullptr;
    }

    g_DispatchParticleAddress = nullptr;
    g_OriginalDispatchParticleEffect = nullptr;
}
