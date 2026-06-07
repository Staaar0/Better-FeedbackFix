using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Core.Attributes;
using CounterStrikeSharp.API.Modules.Admin;
using CounterStrikeSharp.API.Modules.Commands;
using CounterStrikeSharp.API.Modules.UserMessages;
using Microsoft.Extensions.Logging;

namespace BetterFeedbackFix;

public class BetterFeedbackFix : BasePlugin
{
    public override string ModuleName => "BetterFeedbackFix";
    public override string ModuleVersion => "1.0.0";
    public override string ModuleAuthor => "✪ Stαr";
    public override string ModuleDescription =>
        "Hides BetterFeedback custom tracers for suppressed weapons while keeping sound, muzzle flash and impacts.";

    private const int TE_FIRE_BULLETS = 452;

    private const int ITEMDEF_M4A1_S = 60;
    private const int ITEMDEF_USP_S = 61;
    private const int ITEMDEF_MP5SD = 23;

    private const int SOUND_TYPE_SILENCED = 9;

    private const int MODE_SILENCED = 1;

    private const uint INVALID_WEAPON_HANDLE = 0x00FFFFFFu;

    private const string WEAPON_HANDLE_FIELD = "weapon_id";

    private static bool _debug;

    public override void Load(bool hotReload)
    {
        HookUserMessage(TE_FIRE_BULLETS, OnFireBullets, HookMode.Pre);
        Logger.LogInformation("BetterFeedbackFix loaded. Hooking TE_FireBullets (msg id {Id}).", TE_FIRE_BULLETS);
    }

    public override void Unload(bool hotReload)
    {
        UnhookUserMessage(TE_FIRE_BULLETS, OnFireBullets, HookMode.Pre);
    }

    private HookResult OnFireBullets(UserMessage um)
    {
        if (_debug)
            DumpFields(um);

        if (!IsSuppressed(um))
            return HookResult.Continue;

        if (um.HasField(WEAPON_HANDLE_FIELD))
        {
            um.SetUInt(WEAPON_HANDLE_FIELD, INVALID_WEAPON_HANDLE);
            return HookResult.Changed;
        }

        return HookResult.Continue;
    }

    private static bool IsSuppressed(UserMessage um)
    {
        if (!um.HasField("item_def_index"))
            return false;

        int itemDef = um.ReadInt("item_def_index");

        if (itemDef == ITEMDEF_MP5SD)
            return true;

        if (itemDef != ITEMDEF_M4A1_S && itemDef != ITEMDEF_USP_S)
            return false;

        if (um.HasField("sound_type") && um.ReadInt("sound_type") == SOUND_TYPE_SILENCED)
            return true;

        if (um.HasField("mode") && um.ReadInt("mode") == MODE_SILENCED)
            return true;

        return false;
    }

    [ConsoleCommand("css_bffix_debug", "Toggle BetterFeedbackFix field logging (1/0)")]
    [RequiresPermissions("@css/root")]
    [CommandHelper(minArgs: 1, usage: "<1|0>")]
    public void OnDebugCommand(CCSPlayerController? player, CommandInfo info)
    {
        _debug = info.GetArg(1) == "1";
        info.ReplyToCommand($"[BetterFeedbackFix] debug = {_debug}");
    }

    private static readonly string[] ProbeUIntFields =
    {
        "weapon_id", "mode", "seed", "player", "sound_type"
    };

    private static readonly string[] ProbeIntFields =
    {
        "item_def_index"
    };

    private void DumpFields(UserMessage um)
    {
        var parts = new System.Collections.Generic.List<string>();

        foreach (var f in ProbeIntFields)
            if (um.HasField(f))
                parts.Add($"{f}={um.ReadInt(f)}");

        foreach (var f in ProbeUIntFields)
            if (um.HasField(f))
                parts.Add($"{f}={um.ReadUInt(f)} (0x{um.ReadUInt(f):X})");

        Logger.LogInformation("TE_FireBullets: {Fields}", string.Join(", ", parts));
    }
}
