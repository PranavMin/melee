#include "lbneutralspawn.h"

#include <Runtime/platform.h>

#include <melee/gr/forward.h>
#include <melee/pl/forward.h>
#include <melee/pl/player.h>

/* Native port of Slippi Nintendont's Neutral Spawns ("neutral starts"),
 * NeutralSpawn.asm / kernel/gecko/g_mods_*.bin -- a Gecko C2 insert at
 * fn_8016E2BC+0x254 (gmvs.c), i.e. inside vanilla's per-player placement loop,
 * after the spawn point is read and before Player_80032768/Player_80031AD0
 * store it and spawn the fighter from it. lbNeutralSpawn_Override is called
 * from that exact spot and does the same swap. (A post-spawn data hook,
 * rules.on_match_start, cannot reproduce this: by then the fighter is already
 * born at vanilla's coordinate -- the v18/v19 failures.)
 *
 * Table, layouts, ordering and facing rule are transcribed from the decoded
 * payload; see docs/venue-codes-readdressing.md. */

struct NeutralPos {
    f32 x;
    f32 y;
};

struct NeutralStage {
    u16 stkind;
    /* pos[is_teams][order]. Layout 0 alternates sides (P1 far left, P2 far
     * right, P3/P4 inner or centre); layout 1 (teams) puts the first two on
     * one side. */
    struct NeutralPos pos[2][4];
};

/* Payload words 192..295: six entries of {stage id, 8 (x,y) pairs}, -1 ended. */
static const struct NeutralStage neutral_stages[] = {
    { St_Kind_Last, /* Final Destination */
      { { { -60.0f, 10.0f }, { 60.0f, 10.0f }, { -20.0f, 10.0f }, { 20.0f, 10.0f } },
        { { -60.0f, 10.0f }, { -20.0f, 10.0f }, { 60.0f, 10.0f }, { 20.0f, 10.0f } } } },
    { St_Kind_Battle, /* Battlefield */
      { { { -38.8f, 35.2f }, { 38.8f, 35.2f }, { 0.0f, 8.0f }, { 0.0f, 62.4f } },
        { { -38.8f, 35.2f }, { -38.8f, 5.0f }, { 38.8f, 35.2f }, { 38.8f, 5.0f } } } },
    { St_Kind_Story, /* Yoshi's Story */
      { { { -42.0f, 26.6f }, { 42.0f, 28.0f }, { 0.0f, 46.9f }, { 0.0f, 4.9f } },
        { { -42.0f, 26.6f }, { -42.0f, 5.0f }, { 42.0f, 28.0f }, { 42.0f, 5.0f } } } },
    { St_Kind_OldPupupu, /* Dream Land N64 */
      { { { -46.6f, 37.2f }, { 47.4f, 37.3f }, { 0.0f, 7.0f }, { 0.0f, 58.5f } },
        { { -46.6f, 37.2f }, { -46.6f, 5.0f }, { 47.4f, 37.3f }, { 47.4f, 5.0f } } } },
    { St_Kind_Izumi, /* Fountain of Dreams */
      { { { -41.25f, 21.0f }, { 41.25f, 27.0f }, { 0.0f, 5.25f }, { 0.0f, 48.0f } },
        { { -41.25f, 21.0f }, { -41.25f, 5.0f }, { 41.25f, 27.0f }, { 41.25f, 5.0f } } } },
    { St_Kind_PStadium, /* Pokemon Stadium */
      { { { -40.0f, 32.0f }, { 40.0f, 32.0f }, { 70.0f, 7.0f }, { -70.0f, 7.0f } },
        { { -40.0f, 32.0f }, { -40.0f, 5.0f }, { 40.0f, 32.0f }, { 40.0f, 5.0f } } } },
};

#define NEUTRAL_STAGE_COUNT                                                    \
    ((int) (sizeof(neutral_stages) / sizeof(neutral_stages[0])))

static const struct NeutralStage* findStage(u16 stkind)
{
    int i;
    for (i = 0; i < NEUTRAL_STAGE_COUNT; i++) {
        if (neutral_stages[i].stkind == stkind) {
            return &neutral_stages[i];
        }
    }
    return NULL;
}

/* The asm's ordering (payload words 88..111): it builds a list of slots 0..3
 * grouped by slot type -- every Human slot in slot order, then every Cpu,
 * then every Demo -- and a slot's "order" is its index in that list. Slots
 * outside 0..3 (the Nana sub-fighter slots) are never in the list and fall
 * through to vanilla, as the asm's fallback does. */
static int neutralOrder(int slot)
{
    int order = 0;
    int kind;
    int s;

    for (kind = Gm_PKind_Human; kind <= Gm_PKind_Demo; kind++) {
        for (s = 0; s < 4; s++) {
            if (Player_GetPlayerSlotType(s) != kind) {
                continue;
            }
            if (s == slot) {
                return order;
            }
            order++;
        }
    }
    return -1;
}

void lbNeutralSpawn_Override(int slot, u16 stkind, bool is_teams, Vec3* spawn)
{
    const struct NeutralStage* stage = findStage(stkind);
    int order;

    /* Stage not in the table (never the kiosk's six legal stages): the asm
     * keeps a vanilla spawn, which is what *spawn already holds. */
    if (stage == NULL) {
        return;
    }
    order = neutralOrder(slot);
    if (order < 0 || order >= 4) {
        return;
    }

    spawn->x = stage->pos[is_teams ? 1 : 0][order].x;
    spawn->y = stage->pos[is_teams ? 1 : 0][order].y;
    spawn->z = 0.0f;

    /* Facing: toward the centre, from the placed coordinate, strict > as in
     * the asm (the centre spots at x == 0 face right, where vanilla's >= would
     * face left). Set it now so the caller's own facing block, which only
     * runs while the facing is still 0, is skipped. */
    Player_SetFacingDirection(slot, spawn->x > 0.0f ? -1.0f : 1.0f);
}
