#ifndef MELEE_LB_LBTOURNEY_H
#define MELEE_LB_LBTOURNEY_H

#include <Runtime/platform.h>

#include <relay_proto.h>

/* Tournament set state and the CSS score keybinds (design 6.1).
 *
 * Keybinds, active on the CSS only while a set is current:
 *   Z + D-left        P1 wins a game (sends REPORT_SCORE)
 *   Z + D-right       P2 wins a game (sends REPORT_SCORE)
 *   Z + D-down        undo the last game (sends REPORT_SCORE)
 *   Z + Start (1 s)   end the set (sends END_SET; needs a decided score)
 *
 * The score is drawn in the CSS corner as "MANGO 2 - 1 ZAIN", with "!"
 * appended while a request is in flight and "X" (the design's cross) after
 * a failure. Inputs are ignored while a request is in flight. Character
 * ids are the external CharacterKind read from the CSS ckind fields at
 * send time. */

/* Called by mntourney when START_SET succeeds. Copies the set and resets
 * the game list. */
void lbTourney_SetCurrent(const struct set_entry* set);

void lbTourney_ClearCurrent(void);
bool lbTourney_HasCurrent(void);

/* GS_CSS scene hooks (gmscdata rows): run the tournament keybinds, polls
 * and overlay, then the vanilla mnCharSel handler. */
void lbTourney_CSSFrame(void);
void lbTourney_CSSExit(void* arg);

#endif
