#ifndef MELEE_MN_MNTOURNEY_H
#define MELEE_MN_MNTOURNEY_H

#include <Runtime/platform.h>

#include <sysdolphin/baselib/forward.h>

/* Tournament menu (design 6.1): loading screen, set list with L/R
 * first-letter tag filter, confirm screen, error screen with A=retry
 * B=back. Registered as a GS_MENU submenu (MENU_KIND_TOURNAMENT row in
 * mn_803EB6B0); entered by pressing Z on the main menu. */

/* Replaces mn_8022DB10 in mn_803EB6B0[MENU_KIND_MAIN]: enters the
 * Tournament menu on a Z press, otherwise runs the vanilla main-menu
 * think. */
void mnTourney_MainMenuThink(HSD_GObj* gobj);

/* The MENU_KIND_TOURNAMENT think proc. */
void mnTourney_Think(HSD_GObj* gobj);

/* GS_MENU scene on_exit (the vanilla row had none): forgets per-scene SIS
 * objects, which the scene teardown itself frees. */
void mnTourney_MenuSceneExit(void* exit_data);

/* description_indices for the MENU_KIND_TOURNAMENT row. */
extern u16 mnTourney_DescIndices[1];

#endif
