#ifndef MELEE_LB_LBNEUTRALSPAWN_H
#define MELEE_LB_LBNEUTRALSPAWN_H

#include <Runtime/platform.h>

#include <dolphin/mtx.h>

/* Venue "neutral starts": native port of Slippi Nintendont's Neutral Spawns
 * (kernel/gecko/g_mods_*.bin, NeutralSpawn.asm). Called from the VS
 * placement loop in fn_8016E2BC (gmvs.c) right after the vanilla spawn point
 * is read and before the fighter is spawned from it -- the same insertion
 * point as the asm's C2 hook. Replaces *spawn with the stage's fixed neutral
 * coordinate for this slot's order and sets its facing; leaves *spawn alone
 * (vanilla) when the stage is not in the table or the slot is not ranked. */
void lbNeutralSpawn_Override(int slot, u16 stkind, bool is_teams, Vec3* spawn);

#endif
