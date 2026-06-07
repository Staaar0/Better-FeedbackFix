using CounterStrikeSharp.API.Core;
using CounterStrikeSharp.API.Modules.UserMessages;

namespace BetterFeedbackFix;

public class BetterFeedbackFix : BasePlugin
{
    public override string ModuleName => "BetterFeedbackFix";
    public override string ModuleVersion => "1.0.5";
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

    public override void Load(bool hotReload)
    {
        HookUserMessage(TE_FIRE_BULLETS, OnFireBullets, HookMode.Pre);
    }

    public override void Unload(bool hotReload)
    {
        UnhookUserMessage(TE_FIRE_BULLETS, OnFireBullets, HookMode.Pre);
    }

    private HookResult OnFireBullets(UserMessage um)
    {
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
}
