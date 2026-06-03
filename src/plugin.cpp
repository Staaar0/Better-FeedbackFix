#include "plugin.h"

#include <sourcehook.h>
#include <eiface.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <time.h>
    #include <unistd.h>
    #include <sys/mman.h>
    #include <sys/stat.h>
#endif

SilencedTracerBlocker g_SilencedTracerBlocker;
PLUGIN_EXPOSE(SilencedTracerBlocker, g_SilencedTracerBlocker);

static IGameEventManager2 *g_GameEvents = nullptr;
static ISource2Server *g_Source2Server = nullptr;
static std::string g_GameDir;

SH_DECL_HOOK2(IGameEventManager2, FireEvent, SH_NOATTRIB, 0, bool, IGameEvent *, bool);

// CS2's public HL2SDK no longer exposes GAMEEVENTSMANAGER002 through the
// normal engine factory on all builds. CounterStrikeSharp's public test code
// notes that Source2Server vfunc 91 normally returns a pointer that must be
// adjusted by -8 to reach the game-event manager object.
#ifndef INTERFACEVERSION_SERVERGAMEDLL
    #define INTERFACEVERSION_SERVERGAMEDLL "Source2Server001"
#endif

namespace
{
void Log(const char *format, ...);

template <typename T, size_t Index>
T CallVFunc(void *thisptr)
{
    using Fn = T (*)(void *);
    return (*reinterpret_cast<Fn **>(thisptr))[Index](thisptr);
}

#ifdef _WIN32
bool IsReadableMemory(const void *ptr, size_t size = sizeof(void *))
{
    if (!ptr || reinterpret_cast<uintptr_t>(ptr) < 0x10000)
        return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi)))
        return false;

    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD))
        return false;

    const auto begin = reinterpret_cast<uintptr_t>(ptr);
    const auto regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return begin + size <= regionEnd;
}
#else
bool IsReadableMemory(const void *ptr, size_t size = sizeof(void *))
{
    (void)size;
    return ptr && reinterpret_cast<uintptr_t>(ptr) > 0x10000;
}
#endif

bool LooksLikeInterfaceObject(void *ptr)
{
    if (!IsReadableMemory(ptr, sizeof(void *)))
        return false;

    void *vtable = *reinterpret_cast<void **>(ptr);
    return IsReadableMemory(vtable, sizeof(void *) * 8);
}

IGameEventManager2 *TryAcquireGameEventManager(ISmmAPI *ismm)
{
    if (!ismm)
        return nullptr;

    struct FactoryAttempt
    {
        const char *label;
        CreateInterfaceFn factory;
    };

    const FactoryAttempt factories[] = {
        {"engine", ismm->GetEngineFactory()},
        {"server", ismm->GetServerFactory()},
    };

    const char *eventInterfaces[] = {
        "GAMEEVENTSMANAGER002",
        "GAMEEVENTSMANAGER001",
    };

    for (const auto &factory : factories)
    {
        if (!factory.factory)
            continue;

        for (const char *iface : eventInterfaces)
        {
            auto *manager = reinterpret_cast<IGameEventManager2 *>(ismm->VInterfaceMatch(factory.factory, iface, 0));
            if (manager)
            {
                Log("[STB] IGameEventManager2 acquired from %s factory as %s.\n", factory.label, iface);
                return manager;
            }
        }
    }

    g_Source2Server = reinterpret_cast<ISource2Server *>(ismm->VInterfaceMatch(ismm->GetServerFactory(), INTERFACEVERSION_SERVERGAMEDLL, 0));
    if (!g_Source2Server)
    {
        Log("[STB] WARNING: Source2Server001 was not available, cannot use vfunc 91 event fallback.\n");
        return nullptr;
    }

    void *raw = CallVFunc<void *, 91>(g_Source2Server);
    if (!raw)
    {
        Log("[STB] WARNING: ISource2Server vfunc 91 returned null.\n");
        return nullptr;
    }

    auto *manager = reinterpret_cast<IGameEventManager2 *>(reinterpret_cast<uintptr_t>(raw) - 8u);
    if (!LooksLikeInterfaceObject(manager))
    {
        Log("[STB] WARNING: ISource2Server vfunc 91 raw=%p adjusted=%p did not look like an interface object. Not using it.\n", raw, manager);
        return nullptr;
    }

    Log("[STB] IGameEventManager2 acquired from ISource2Server vfunc 91 raw=%p adjusted=%p.\n", raw, manager);
    return manager;
}

constexpr int kBlockWindowMs = 350;
constexpr int kTracerBlocksPerShot = 12;
constexpr int kMaxPendingBlocks = 64;
#ifdef _WIN32
// The Windows DispatchParticleEffect prologue in current server.dll has a
// 15-byte instruction boundary before the next instruction. Using 16 would
// split the following instruction and can crash when the trampoline returns.
constexpr size_t kPatchSize = 15;
#else
constexpr size_t kPatchSize = 16;
#endif

std::atomic<long long> g_BlockUntilMs{0};
std::atomic<int> g_PendingTracerBlocks{0};
std::atomic<int> g_DebugWeaponLogs{0};
std::atomic<int> g_DebugBlockLogs{0};
std::atomic<int> g_DebugParticleLogs{0};
std::atomic<int> g_DebugAllWeaponLogs{0};
bool g_FireEventHooked = false;
bool g_DetourAttempted = false;
bool g_DetourInstalled = false;
std::vector<int> g_DispatchPattern;

using DispatchParticleEffectFn = void (*)(const char *particleName,
                                         int attachType,
                                         void *entity,
                                         char attachmentPoint,
                                         void *attachmentName,
                                         bool resetAllParticlesOnEntity,
                                         int splitScreenPlayerSlot,
                                         void *recipientFilter,
                                         unsigned char *unknown);

DispatchParticleEffectFn g_OriginalDispatchParticleEffect = nullptr;
void *g_DispatchParticleAddress = nullptr;
void *g_Trampoline = nullptr;
unsigned char g_OriginalBytes[kPatchSize]{};

long long NowMs()
{
#ifdef _WIN32
    return static_cast<long long>(GetTickCount64());
#else
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000LL + ts.tv_nsec / 1000000LL;
#endif
}

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

    // Always write a local fallback, then write into the actual game/csgo tree
    // when MetaMod provides the absolute base directory.
    AppendLogFile("stb_debug.log", buffer);

    if (!g_GameDir.empty())
    {
        const std::string addonDir = GamePath("addons/SilencedTracerBlocker");
        EnsureDirectoryTree(addonDir.c_str());
        AppendLogFile(GamePath("stb_debug.log").c_str(), buffer);
        AppendLogFile(GamePath("addons/SilencedTracerBlocker/stb_debug.log").c_str(), buffer);
    }
}

void LogCurrentWorkingDirectory()
{
    const std::string cwd = GetCurrentWorkingDirectoryString();
    if (!cwd.empty())
        Log("[STB] Current working directory: %s\n", cwd.c_str());
    if (!g_GameDir.empty())
        Log("[STB] Game directory: %s\n", g_GameDir.c_str());
}

std::string Trim(std::string value)
{
    const char *spaces = " \t\r\n";
    const auto begin = value.find_first_not_of(spaces);
    if (begin == std::string::npos)
        return {};

    const auto end = value.find_last_not_of(spaces);
    return value.substr(begin, end - begin + 1);
}

std::string LowerCopy(std::string_view value)
{
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::vector<int> ParsePattern(const std::string &text)
{
    std::vector<int> out;
    std::istringstream iss(text);
    std::string token;

    while (iss >> token)
    {
        if (token == "?" || token == "??" || token == "2A")
            out.push_back(-1);
        else
            out.push_back(static_cast<int>(std::strtol(token.c_str(), nullptr, 16)));
    }

    return out;
}

bool IsSilencedWeaponName(std::string_view weaponName)
{
    const std::string weapon = LowerCopy(weaponName);

    // CS2 should provide weapon_fire.silenced, but live servers/plugins can
    // report it inconsistently. BetterFeedback's issue is with the weapon
    // tracer particle generated by the suppressed weapon classes, so treat
    // these classes as a safe fallback trigger. This keeps AK/M4A4/AWP/etc.
    // untouched while fixing USP-S/M4A1-S.
    return weapon == "weapon_usp_silencer" ||
           weapon == "usp_silencer" ||
           weapon == "weapon_m4a1_silencer" ||
           weapon == "m4a1_silencer" ||
           weapon == "weapon_mp5sd" ||
           weapon == "mp5sd";
}

bool IsDefinitelySilencedShot(IGameEvent *event)
{
    if (!event)
        return false;

    if (event->GetBool("silenced", false))
        return true;

    return IsSilencedWeaponName(event->GetString("weapon", ""));
}

bool IsTracerParticleName(const char *particleName)
{
    if (!particleName || !particleName[0])
        return false;

    const std::string name = LowerCopy(particleName);

    // BetterFeedback replaces the shared tracer particles, so match tracer
    // names broadly but explicitly keep impact/blood/ricochet/taser particles.
    if (name.find("tracer") == std::string::npos)
        return false;

    if (name.find("impact") != std::string::npos ||
        name.find("blood") != std::string::npos ||
        name.find("decal") != std::string::npos ||
        name.find("ricochet") != std::string::npos ||
        name.find("taser") != std::string::npos)
    {
        return false;
    }

    return true;
}

void MarkSilencedShot()
{
    g_BlockUntilMs.store(NowMs() + kBlockWindowMs);

    int current = g_PendingTracerBlocks.load();
    while (current < kMaxPendingBlocks)
    {
        const int desired = (current + kTracerBlocksPerShot > kMaxPendingBlocks)
                                ? kMaxPendingBlocks
                                : current + kTracerBlocksPerShot;
        if (g_PendingTracerBlocks.compare_exchange_weak(current, desired))
            break;
    }
}

void Detour_DispatchParticleEffect(const char *particleName,
                                  int attachType,
                                  void *entity,
                                  char attachmentPoint,
                                  void *attachmentName,
                                  bool resetAllParticlesOnEntity,
                                  int splitScreenPlayerSlot,
                                  void *recipientFilter,
                                  unsigned char *unknown)
{
    const bool isTracer = IsTracerParticleName(particleName);
    if (isTracer)
    {
        const int logIndex = g_DebugParticleLogs.fetch_add(1);
        if (logIndex < 32)
        {
            Log("[STB] Tracer particle observed: name=%s pending=%d active=%s\n",
                particleName ? particleName : "<null>",
                g_PendingTracerBlocks.load(),
                NowMs() <= g_BlockUntilMs.load() ? "yes" : "no");
        }
    }

    if (isTracer && g_SilencedTracerBlocker.ConsumeTracerBlockToken())
    {
        const int logIndex = g_DebugBlockLogs.fetch_add(1);
        if (logIndex < 32)
            Log("[STB] Blocked silenced tracer particle: %s\n", particleName ? particleName : "<null>");
        return;
    }

    if (!g_OriginalDispatchParticleEffect)
        return;

    g_OriginalDispatchParticleEffect(particleName,
                                     attachType,
                                     entity,
                                     attachmentPoint,
                                     attachmentName,
                                     resetAllParticlesOnEntity,
                                     splitScreenPlayerSlot,
                                     recipientFilter,
                                     unknown);
}

void *PatternScan(uintptr_t base, size_t size, const std::vector<int> &pattern)
{
    if (pattern.empty() || size < pattern.size())
        return nullptr;

    auto *data = reinterpret_cast<unsigned char *>(base);

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

#ifdef _WIN32
bool FindServerModuleAndScan(void *&address)
{
    HMODULE module = GetModuleHandleA("server.dll");
    if (!module)
        return false;

    auto *base = reinterpret_cast<unsigned char *>(module);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    address = PatternScan(reinterpret_cast<uintptr_t>(base),
                          static_cast<size_t>(nt->OptionalHeader.SizeOfImage),
                          g_DispatchPattern);
    return address != nullptr;
}

bool ProtectMemory(void *address, size_t size, DWORD newProtection, DWORD &oldProtection)
{
    return VirtualProtect(address, size, newProtection, &oldProtection) != 0;
}

void FlushCodeCache(void *address, size_t size)
{
    FlushInstructionCache(GetCurrentProcess(), address, size);
}

void *AllocateExecutable(size_t size)
{
    return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

void FreeExecutable(void *address, size_t)
{
    if (address)
        VirtualFree(address, 0, MEM_RELEASE);
}
#else
bool FindServerModuleAndScan(void *&address)
{
    std::ifstream maps("/proc/self/maps");
    std::string line;

    while (std::getline(maps, line))
    {
        if (line.find("server.so") == std::string::npos)
            continue;

        // Scan executable readable mappings only.
        if (line.find("r-x") == std::string::npos && line.find("r--") == std::string::npos)
            continue;

        uintptr_t start = 0;
        uintptr_t end = 0;
        if (std::sscanf(line.c_str(), "%lx-%lx", &start, &end) != 2 || end <= start)
            continue;

        address = PatternScan(start, end - start, g_DispatchPattern);
        if (address)
            return true;
    }

    return false;
}

uintptr_t PageStart(uintptr_t address)
{
    const auto pageSize = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    return address & ~(pageSize - 1);
}

size_t PageSpan(void *address, size_t size)
{
    const auto pageSize = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    const uintptr_t start = PageStart(reinterpret_cast<uintptr_t>(address));
    const uintptr_t end = PageStart(reinterpret_cast<uintptr_t>(address) + size - 1) + pageSize;
    return static_cast<size_t>(end - start);
}

bool ProtectMemory(void *address, size_t size, int protection)
{
    void *page = reinterpret_cast<void *>(PageStart(reinterpret_cast<uintptr_t>(address)));
    return mprotect(page, PageSpan(address, size), protection) == 0;
}

void FlushCodeCache(void *address, size_t size)
{
    __builtin___clear_cache(reinterpret_cast<char *>(address), reinterpret_cast<char *>(address) + size);
}

void *AllocateExecutable(size_t size)
{
    void *memory = mmap(nullptr,
                        size,
                        PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS,
                        -1,
                        0);
    return memory == MAP_FAILED ? nullptr : memory;
}

void FreeExecutable(void *address, size_t size)
{
    if (address)
        munmap(address, size);
}
#endif

bool WriteJump(void *from, void *to)
{
#ifdef _WIN32
    DWORD oldProtection = 0;
    if (!ProtectMemory(from, kPatchSize, PAGE_EXECUTE_READWRITE, oldProtection))
        return false;
#else
    if (!ProtectMemory(from, kPatchSize, PROT_READ | PROT_WRITE | PROT_EXEC))
        return false;
#endif

    unsigned char patch[kPatchSize]{};
    patch[0] = 0x48; // mov rax, imm64
    patch[1] = 0xB8;
    *reinterpret_cast<uint64_t *>(&patch[2]) = reinterpret_cast<uint64_t>(to);
    patch[10] = 0xFF; // jmp rax
    patch[11] = 0xE0;

    for (size_t i = 12; i < kPatchSize; ++i)
        patch[i] = 0x90;

    std::memcpy(from, patch, kPatchSize);
    FlushCodeCache(from, kPatchSize);

#ifdef _WIN32
    DWORD ignored = 0;
    VirtualProtect(from, kPatchSize, oldProtection, &ignored);
#else
    ProtectMemory(from, kPatchSize, PROT_READ | PROT_EXEC);
#endif

    return true;
}
} // namespace

bool SilencedTracerBlocker::Load(PluginId id, ISmmAPI *ismm, char *error, size_t maxlen, bool late)
{
    PLUGIN_SAVEVARS();
    SetGameDir(ismm);

    Log("[STB] Load() entered. version=%s late=%s\n", GetVersion(), late ? "true" : "false");
    LogCurrentWorkingDirectory();

    g_GameEvents = TryAcquireGameEventManager(ismm);
    if (!g_GameEvents)
    {
        Log("[STB] WARNING: Could not acquire IGameEventManager2. Plugin will still load for particle diagnostics, but silenced-shot detection will be disabled.\n");
    }

    char diagError[512]{};
    bool gamedataOk = LoadGameData(diagError, sizeof(diagError));
    bool detourOk = false;

    if (!gamedataOk)
    {
        Log("[STB] WARNING: gamedata not loaded: %s. Continuing event diagnostics only.\n", diagError);
    }
    else
    {
        Log("[STB] Gamedata loaded. Particle detour will be installed lazily on the first silenced weapon_fire to avoid startup crashes.\n");
    }

    bool listenerOk = false;
    bool fireEventHookOk = false;

    if (g_GameEvents)
    {
        // Use the normal game-event listener first. Do not SourceHook FireEvent
        // here; an invalid CS2 event-manager pointer can hard-crash the server.
        listenerOk = g_GameEvents->AddListener(this, "weapon_fire", true);
    }

    Log("[STB] SilencedTracerBlocker loaded. impacts/blood untouched. FireEventHook=%s ListenerWeaponFire=%s GameData=%s ParticleDetour=%s\n",
        fireEventHookOk ? "on" : "off",
        listenerOk ? "ok" : "off",
        gamedataOk ? "ok" : "failed",
        detourOk ? "ok" : "lazy");
    return true;
}

bool SilencedTracerBlocker::Unload(char *error, size_t maxlen)
{
    if (g_GameEvents)
    {
        if (g_FireEventHooked)
        {
            SH_REMOVE_HOOK_MEMFUNC(IGameEventManager2, FireEvent, g_GameEvents, this, &SilencedTracerBlocker::Hook_FireEvent, false);
            g_FireEventHooked = false;
        }
        g_GameEvents->RemoveListener(this);
    }

    RemoveDispatchDetour();
    g_PendingTracerBlocks.store(0);
    g_BlockUntilMs.store(0);

    Log("[STB] SilencedTracerBlocker unloaded.\n");
    return true;
}

void SilencedTracerBlocker::FireGameEvent(IGameEvent *event)
{
    HandleGameEvent(event, "listener");
}

bool SilencedTracerBlocker::Hook_FireEvent(IGameEvent *event, bool bDontBroadcast)
{
    HandleGameEvent(event, "fireevent-hook");
    RETURN_META_VALUE(MRES_IGNORED, true);
}

void SilencedTracerBlocker::HandleGameEvent(IGameEvent *event, const char *source)
{
    if (!event)
        return;

    const char *eventName = event->GetName();
    if (!eventName || std::strcmp(eventName, "weapon_fire") != 0)
        return;

    const int seenIndex = g_DebugAllWeaponLogs.fetch_add(1);
    if (seenIndex < 32)
    {
        Log("[STB] weapon_fire seen via %s: weapon=%s silenced=%s userid=%d\n",
            source ? source : "unknown",
            event->GetString("weapon", ""),
            event->GetBool("silenced", false) ? "true" : "false",
            event->GetInt("userid", 0));
    }

    if (IsDefinitelySilencedShot(event))
    {
        const int logIndex = g_DebugWeaponLogs.fetch_add(1);
        if (logIndex < 32)
        {
            Log("[STB] Silenced weapon_fire detected via %s: weapon=%s silenced=%s\n",
                source ? source : "unknown",
                event->GetString("weapon", ""),
                event->GetBool("silenced", false) ? "true" : "false");
        }

        if (!g_DetourAttempted && !g_DetourInstalled && !g_DispatchPattern.empty())
        {
            g_DetourAttempted = true;
            char detourError[512]{};
            g_DetourInstalled = InstallDispatchDetour(detourError, sizeof(detourError));
            if (!g_DetourInstalled)
                Log("[STB] WARNING: lazy particle detour failed: %s\n", detourError);
        }

        MarkSilencedShot();
    }
}

bool SilencedTracerBlocker::ConsumeTracerBlockToken()
{
    if (NowMs() > g_BlockUntilMs.load())
    {
        g_PendingTracerBlocks.store(0);
        return false;
    }

    int current = g_PendingTracerBlocks.load();
    while (current > 0)
    {
        if (g_PendingTracerBlocks.compare_exchange_weak(current, current - 1))
            return true;
    }

    return false;
}

bool SilencedTracerBlocker::LoadGameData(char *error, size_t maxlen)
{
    std::string path = GamePath("addons/SilencedTracerBlocker/gamedata/silenced_tracer_blocker.games.txt");
    std::ifstream file(path);

    if (!file.is_open())
    {
        // Also accept lowercase folder names from older test packages.
        path = GamePath("addons/silencedtracerblocker/gamedata/silenced_tracer_blocker.games.txt");
        file.open(path);
    }

    if (!file.is_open())
    {
        std::snprintf(error, maxlen, "Could not open %s", path.c_str());
        return false;
    }

    Log("[STB] Loading gamedata from %s\n", path.c_str());

#ifdef _WIN32
    constexpr std::string_view wantedKey = "DispatchParticleEffect_Windows";
#else
    constexpr std::string_view wantedKey = "DispatchParticleEffect_Linux";
#endif

    std::string genericPattern;
    std::string platformPattern;
    std::string line;

    while (std::getline(file, line))
    {
        const auto comment = line.find("//");
        if (comment != std::string::npos)
            line.erase(comment);

        const auto equals = line.find('=');
        if (equals == std::string::npos)
            continue;

        const std::string key = Trim(line.substr(0, equals));
        const std::string value = Trim(line.substr(equals + 1));

        if (key == wantedKey)
            platformPattern = value;
        else if (key == "DispatchParticleEffect")
            genericPattern = value;
    }

    const std::string selectedPattern = !platformPattern.empty() ? platformPattern : genericPattern;
    g_DispatchPattern = ParsePattern(selectedPattern);

    if (g_DispatchPattern.empty())
    {
#ifdef _WIN32
        std::snprintf(error, maxlen, "Windows DispatchParticleEffect signature is missing from gamedata");
#else
        std::snprintf(error, maxlen, "Linux DispatchParticleEffect signature is missing from gamedata");
#endif
        return false;
    }

    return true;
}

bool SilencedTracerBlocker::InstallDispatchDetour(char *error, size_t maxlen)
{
    if (!FindServerModuleAndScan(g_DispatchParticleAddress))
    {
        std::snprintf(error, maxlen, "DispatchParticleEffect signature not found in server module");
        return false;
    }

    std::memcpy(g_OriginalBytes, g_DispatchParticleAddress, kPatchSize);

    constexpr size_t trampolineSize = kPatchSize + kPatchSize;
    g_Trampoline = AllocateExecutable(trampolineSize);
    if (!g_Trampoline)
    {
        std::snprintf(error, maxlen, "Failed to allocate detour trampoline");
        return false;
    }

    std::memcpy(g_Trampoline, g_OriginalBytes, kPatchSize);

    void *returnAddress = reinterpret_cast<unsigned char *>(g_DispatchParticleAddress) + kPatchSize;
    if (!WriteJump(reinterpret_cast<unsigned char *>(g_Trampoline) + kPatchSize, returnAddress))
    {
        std::snprintf(error, maxlen, "Failed to write trampoline jump");
        return false;
    }

    g_OriginalDispatchParticleEffect = reinterpret_cast<DispatchParticleEffectFn>(g_Trampoline);

    if (!WriteJump(g_DispatchParticleAddress, reinterpret_cast<void *>(&Detour_DispatchParticleEffect)))
    {
        std::snprintf(error, maxlen, "Failed to write DispatchParticleEffect detour");
        return false;
    }

    g_DetourInstalled = true;
    Log("[STB] DispatchParticleEffect detoured at %p. PatchSize=%zu\n", g_DispatchParticleAddress, kPatchSize);
    return true;
}

void SilencedTracerBlocker::RemoveDispatchDetour()
{
    if (g_DispatchParticleAddress)
    {
#ifdef _WIN32
        DWORD oldProtection = 0;
        if (ProtectMemory(g_DispatchParticleAddress, kPatchSize, PAGE_EXECUTE_READWRITE, oldProtection))
        {
            std::memcpy(g_DispatchParticleAddress, g_OriginalBytes, kPatchSize);
            FlushCodeCache(g_DispatchParticleAddress, kPatchSize);
            DWORD ignored = 0;
            VirtualProtect(g_DispatchParticleAddress, kPatchSize, oldProtection, &ignored);
        }
#else
        if (ProtectMemory(g_DispatchParticleAddress, kPatchSize, PROT_READ | PROT_WRITE | PROT_EXEC))
        {
            std::memcpy(g_DispatchParticleAddress, g_OriginalBytes, kPatchSize);
            FlushCodeCache(g_DispatchParticleAddress, kPatchSize);
            ProtectMemory(g_DispatchParticleAddress, kPatchSize, PROT_READ | PROT_EXEC);
        }
#endif
    }

    constexpr size_t trampolineSize = kPatchSize + kPatchSize;
    FreeExecutable(g_Trampoline, trampolineSize);

    g_Trampoline = nullptr;
    g_DispatchParticleAddress = nullptr;
    g_OriginalDispatchParticleEffect = nullptr;
    g_DetourInstalled = false;
}
