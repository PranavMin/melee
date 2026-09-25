/* Tournament module glue: what the kiosk TUs need from vanilla Melee that is
 * not an exported function. The module is linked against the VANILLA symbol
 * map (config/GALE01/symbols.txt), so file-static variables of the vanilla
 * build are reachable here as absolute symbols; this file declares them with
 * their real types and wraps the few reads/writes the kiosk makes.
 *
 * Only compiled into the module (tools/build_module.py), never into the DOL. */
#include <Runtime/platform.h>

#include <melee/gm/gm_1A3F.h>
#include <melee/gm/forward.h>
#include <melee/gm/types.h>
#include <melee/mn/forward.h>
#include <melee/mn/mncharsel.h>
#include <melee/mn/types.h>
#include <melee/pl/forward.h>

/* mncharsel.c statics (vanilla addresses via symbols.txt: .sbss 0x804D6CB0,
 * 0x804D6CF2/F6/F7). */
extern CSSData* mnCharSel_804D6CB0;
extern u8 mnCharSel_804D6CF2; /* post-confirm lockout */
extern u8 mnCharSel_804D6CF6; /* CSSPendingSceneChangeKind: 1 = go fight */
extern u8 mnCharSel_804D6CF7; /* every present player is ready */

u8 mnCharSel_PortNametag(int port)
{
    if (port < 0 || port >= 4 || mnCharSel_804D6CB0 == NULL) {
        return 0x78;
    }
    return mnCharSel_804D6CB0->vs.start.players[port].nametag;
}

u8 mnCharSel_PortSlotType(int port)
{
    if (port < 0 || port >= 4 || mnCharSel_804D6CB0 == NULL) {
        return Gm_PKind_NA;
    }
    return mnCharSel_804D6CB0->vs.start.players[port].slot_type;
}

bool mnCharSel_TryStartFight(void)
{
    if (mnCharSel_804D6CF2 != 0 || mnCharSel_804D6CF7 == 0) {
        return false;
    }
    mnCharSel_804D6CF6 = 1;
    mnCharSel_804D6CF2 = 0xFF;
    return true;
}

/* Boot hook: the module's `bootOnLoad` replacement (gmboot.c), reached by a
 * branch patched over the vanilla function's first instruction. Skips the
 * opening movie and title screen, landing on GM_MENU, whose main-menu think
 * (mnTourney_MainMenuThink, installed by a table patch) auto-enters the set
 * list. */
struct tm_bootLoadData {
    u32 x0;
    u8 x4;
    u8 mode_id;
};

void tm_bootOnLoad(GameModeState* scene)
{
    struct tm_bootLoadData* d = gm_GetGameModeStateEnterData(scene);
    d->x4 = 0;
    d->x0 = 0;
    d->mode_id = GM_MENU;
}
