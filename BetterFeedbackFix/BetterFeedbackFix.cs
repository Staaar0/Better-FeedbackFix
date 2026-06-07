using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.Admin;
using CounterStrikeSharp.API.Modules.Commands;
using CounterStrikeSharp.API.Modules.UserMessages;
using Microsoft.Extensions.Logging;

namespace BetterFeedbackFix;

// CounterStrikeSharp port of the "Better-FeedbackFix" Metamod plugin.
//
// Original behaviour:
//   The Metamod plugin hooked GameEventSystemServerV001::PostEventAbstract and,
//   for the TE_FireBullets broadcast (CMsgTEFireBullets), overwrote the weapon
//   entity handle with an invalid handle (0x00FFFFFF) when the shot came from a
//   suppressed M4A1-S / USP-S / MP5-SD. The Better Feedback workshop addon draws
//   its custom tracer off that weapon entity, so invalidating it hides the tracer
//   while leaving sound / muzzle flash / impacts untouched.
//
// This port reproduces the same logic with CounterStrikeSharp's user-message hook,
// which intercepts the same TE_FireBullets broadcast and edits protobuf fields by name.
public class BetterFeedbackFix : BasePlugin
{
    public override string ModuleName => "BetterFeedbackFix";
    public override string ModuleVersion => "1.0.0";
    public override string ModuleAuthor => "\u272a St\u03b1r (CSSharp port)";
    public override string ModuleDescription =>
        "Hides BetterFeedback custom tracers for suppressed weapons while keeping sound, muzzle flash and impacts.";

    // Network message id for TE_FireBullets (CMsgTEFireBullets).
    // Same value the original Metamod plugin used (kFireBulletsId = 452).
    // If a future game update shifts message ids, see the debug command below to confirm.
    private const int TE_FIRE_BULLETS = 452;

    // Item definition indices.
    private const int ITEMDEF_M4A1_S = 60;
    private const int ITEMDEF_USP_S = 61;
    private const int ITEMDEF_MP5SD = 23;

    // sound_type value emitted when a toggle-silencer weapon fires while suppressed.
    private const int SOUND_TYPE_SILENCED = 9;

    // mode flag set by toggle-silencer weapons (M4A1-S / USP-S) when the silencer is attached.
    private const int MODE_SILENCED = 1;

    // Invalid CEntityHandle. Mirrors kDefaultEntityHandle (0x00FFFFFF) from the original plugin.
    private const uint INVALID_WEAPON_HANDLE = 0x00FFFFFFu;

    // The serialized field in CMsgTEFireBullets that references the firing weapon entity.
    // This is the protobuf equivalent of the handle the Metamod plugin overwrote.
    // If on your build this also affects muzzle flash, enable debug (css_bffix_debug 1)
    // and compare a silenced vs. unsilenced shot to find the right field, then change this.
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

        // Neutralise the weapon entity reference so the custom tracer has nothing
        // to attach to. Origin / sound_type / player are left alone, so the shot
        // sound, muzzle flash and bullet impacts are unaffected.
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

        // MP5-SD is permanently suppressed.
        if (itemDef == ITEMDEF_MP5SD)
            return true;

        // Only the toggle-silencer pistols/rifle are relevant beyond this point.
        if (itemDef != ITEMDEF_M4A1_S && itemDef != ITEMDEF_USP_S)
            return false;

        // Confirm the silencer is actually attached (otherwise leave the tracer alone).
        if (um.HasField("sound_type") && um.ReadInt("sound_type") == SOUND_TYPE_SILENCED)
            return true;

        if (um.HasField("mode") && um.ReadInt("mode") == MODE_SILENCED)
            return true;

        return false;
    }

    // ---- Debug helper -------------------------------------------------------
    // Toggle from server console: css_bffix_debug 1   (and 0 to turn off)
    // Fire one shot silenced and one unsilenced, then diff the logged values to
    // confirm which field carries the weapon handle on your game build.
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
