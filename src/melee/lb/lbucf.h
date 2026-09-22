#ifndef MELEE_LB_LBUCF_H
#define MELEE_LB_LBUCF_H

#include <Runtime/platform.h>

/* UCF 0.8 (Universal Controller Fix), ported natively from Slippi Nintendont's
 * g_ucf.bin. Three fixes -- dashback, wiggle-out-of-tumble, shield-drop --
 * each transcribed from its decoded C2 payload (see docs/ucf-investigation.md
 * and the comments in lbucf.c). Delivered as thin IASA wrappers installed by
 * swapping input_cb pointers in the source data table ftData_MotionStateList
 * (the same data-table hook pattern as the rest of the tournament code); the
 * vanilla IASAs are only ever called, never edited. The cosmetic "UCF 0.8" CSS
 * label of the original code set is intentionally not carried. */
void lbUcf_Install(void);

#endif
