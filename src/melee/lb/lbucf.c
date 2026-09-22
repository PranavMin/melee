#include "lbucf.h"

#include <Runtime/platform.h>

#include <melee/ft/fighter.h>
#include <melee/ft/forward.h>
#include <melee/ft/ftdata.h>
#include <melee/ft/inlines.h>
#include <melee/ft/types.h>
#include <melee/ft/kinds/ftCommon/forward.h>
#include <melee/ft/kinds/ftCommon/ftCo_AppealS.h>
#include <melee/ft/kinds/ftCommon/ftCo_DamageFall.h>
#include <melee/ft/kinds/ftCommon/ftCo_Guard.h>
#include <melee/ft/kinds/ftCommon/ftCo_Turn.h>
#include <melee/ft/kinds/ftCommon/ftCo_Wait.h>
#include <melee/pl/player.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/forward.h>
#include <sysdolphin/baselib/gobj.h>

/* Native port of UCF 0.8 (Slippi Nintendont kernel/gecko/g_ucf.bin). Each fix
 * below is a transcription of its decoded C2 payload -- same conditions, same
 * constants, same effect on the same fighter state -- delivered as a thin
 * wrapper around the untouched vanilla IASA, installed by swapping input_cb
 * pointers in ftData_MotionStateList (a source data table; no matched
 * function is edited). Where the asm reaches inside a matched function to fake
 * a compare result, the wrapper instead sets the operand that compare reads
 * (a window constant) for the duration of the call and restores it.
 *
 * Known gap vs the asm (documented, not hidden): ftCo_Wait_IASA and
 * ftCo_DamageFall_IASA are also called *directly* from other states
 * (ftCo_Attack1/AttackDash/AttackHi3/AttackHi4/AttackLw4/AttackS3, ftCo_Damage,
 * ftCo_ItemParasolDamageFall). Those calls bypass the table and so bypass the
 * shield-drop / tumble wrappers; the C2 hooks, sitting inside the callee, did
 * not. Wrap those callers' rows too if that ever matters. */

/* ---- raw stick, exactly as the asm reads it ------------------------------ */

#define UCF_FLICK_SQ 5625 /* 75^2: the payloads' raw-delta threshold */
#define UCF_POLL_RING 5   /* gmMain_8046B108[5]; the asm hardcodes the wrap */

/* The asm's helper: given a ring index, read the poll BEFORE it (index - 1,
 * wrapped once at 5) for `port`, returning the raw SI stickX (s8, about +-80).
 * Read through the extern HSD_PadLibData (qread / queue): it is the very ring
 * gmmain.c registers with HSD_PadInit(5, gmMain_8046B108, ...), which the
 * dashback payload names directly. */
static int rawStickX(int poll, int port)
{
    PadLibData* p = &HSD_PadLibData;
    poll -= 1;
    if (poll < 0) {
        poll += UCF_POLL_RING;
    }
    return p->queue[poll].stat[port].stickX;
}

/* (X[now-1] - X[now-3])^2 > 75^2. The asm passes qread and qread-2 to the
 * helper, which reads one poll earlier still. */
static bool fastFlick(int port)
{
    int now = HSD_PadLibData.qread;
    int older = rawStickX(now - 2, port);
    int newer = rawStickX(now, port);
    int d = newer - older;
    return (d * d) > UCF_FLICK_SQ;
}

/* ---- dashback: C2 at ftCo_Turn_IASA+0x4C -------------------------------- */

static void ucf_Turn_IASA(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    ftCommonData* cd = p_ftCommonData;
    bool orig_just_turned = fp->mv.co.turn.just_turned;
    HSD_Pad saved_x1C = fp->mv.co.turn.x1C;
    bool fired = false;

    /* The hook replaces vanilla's first `facing_dir = -facing_dir` store
     * (only reached while !has_turned) and re-executes it. Then, on Turn's
     * anim frame 2, with the stick past the dash threshold, the smash timer
     * still < 2, not a sub-fighter, and a raw flick over 75 in two polls, it
     * commits the turn (has_turned = 1, so vanilla's un-flip is skipped) and
     * marks just_turned so the dash branch at the end of the IASA fires. */
    if (!fp->mv.co.turn.has_turned && fp->cur_anim_frame == 2.0f &&
        ABS(fp->input.lstick[0].x) >= cd->dash_smash_stick_threshold &&
        fp->active_timer.lstick.x < 2 && !fp->is_sub_fighter &&
        fastFlick(fp->x618_player_id))
    {
        fp->facing_dir = -fp->facing_dir; /* vanilla's first flip, taken now */
        fp->mv.co.turn.has_turned = true;
        fp->mv.co.turn.just_turned = true;
        fired = true;

        /* Ice Climbers: a Popo's Nana (entity 1) follows the dashback. The
         * asm writes her recorded-input record (cpu.x444): the new facing,
         * and the stick at full deflection toward it. */
        if (fp->kind == Ft_Kind_Popo) {
            HSD_GObj* nana = Player_GetEntityAtIndex(fp->player_id, 1);
            if (nana != NULL) {
                Fighter* nfp = GET_FIGHTER(nana);
                struct Fighter_x1A88_xFC_t* rec = nfp->cpu.x444;
                if (rec != NULL) {
                    rec->facing_dir = fp->facing_dir;
                    rec->lstick.x = fp->facing_dir > 0.0f ? 0x7F : (s8) 0x80;
                }
            }
        }
    }

    /* Vanilla ORs x1C into pressed_buttons at its very top when just_turned
     * is set -- a line that runs BEFORE the hook point. When we are the ones
     * setting just_turned, keep that OR a no-op for this call so the button
     * buffer behaves exactly as on the asm path. */
    if (fired && !orig_just_turned) {
        fp->mv.co.turn.x1C = 0;
    }
    ftCo_Turn_IASA(gobj);
    if (fired && !orig_just_turned) {
        fp->mv.co.turn.x1C |= saved_x1C;
    }
}

/* ---- wiggle out of tumble: C2 at ftCo_DamageFall_IASA+0xCC --------------- */

static void ucf_DamageFall_IASA(HSD_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    ftCommonData* cd = p_ftCommonData;
    int saved_x214 = cd->x214;

    /* The hook replaces vanilla's `active_timer.lstick.x < x214` compare with
     * its own answer: timer 0 -> wiggle (as vanilla); timer >= 2 -> no (x214
     * is never consulted); timer 1 -> rescued only if the previous frame's
     * stick was not yet past the threshold (a fresh crossing) and the raw
     * stick flicked over 75 in two polls. Give vanilla's own compare a window
     * that yields that answer, and restore it after. Vanilla only reaches the
     * compare when the stick is past x210, so gate the same way. */
    if (ABS(fp->input.lstick[0].x) >= cd->x210) {
        u8 t = fp->active_timer.lstick.x;
        bool allow;
        if (t == 0) {
            allow = true;
        } else if (t == 1) {
            allow = ABS(fp->input.lstick[1].x) < cd->x210 &&
                    fastFlick(fp->x618_player_id);
        } else {
            allow = false;
        }
        cd->x214 = allow ? 255 : 0;
    }
    ftCo_DamageFall_IASA(gobj);
    cd->x214 = saved_x214;
}

/* ---- shield drop: C2 at ftCo_80099894+0x10 ------------------------------- */

static f32 ucf_Eps(void)
{
    union {
        u32 u;
        f32 f;
    } c;
    c.u = 0x37270000; /* about 9.95e-6, the payload's constant word */
    return c.f;
}

/* trunc(|v| * 80 - eps) + 2, over 80: the asm's quantisation of a stick
 * coordinate (fmuls, fsubs, fctiwz, int-to-float, fdivs). */
static f32 ucf_Quantize(f32 v)
{
    f32 a = ABS(v) * 80.0f - ucf_Eps();
    int n = (int) a;
    return (f32) (n + 2) / 80.0f;
}

/* True when the asm would veto: the drop is c-stick-initiated (cstick.y
 * below x314) and none of its three escapes hold -- quantised stick
 * magnitude under 1.0, X smash timer <= 3 (the X timer: faithful to the
 * bytes), or lstick.y at or below -0.8. */
static bool ucf_ShieldDropVeto(Fighter* fp)
{
    ftCommonData* cd = p_ftCommonData;
    f32 qx;
    f32 qy;

    if (!(fp->input.cstick[0].y < cd->x314)) {
        return false;
    }
    qx = ucf_Quantize(fp->input.lstick[0].x);
    qy = ucf_Quantize(fp->input.lstick[0].y);
    if (qx * qx + qy * qy < 1.0f) {
        return false;
    }
    if (fp->active_timer.lstick.x <= 3) {
        return false;
    }
    if (fp->input.lstick[0].y <= -0.8f) {
        return false;
    }
    return true;
}

/* The asm vetoes inside the drop executor and makes its entry (ftCo_8009980C
 * / ftCo_80099794) return false. Both entries test against x314 (analog:
 * lstick.y <= x314 && timer.y < x318; c-stick: cstick.y <= x314), so an
 * out-of-reach x314 for the duration of this IASA call makes both false
 * without touching the input buffers. */
static void ucf_DropGuardedIASA(HSD_GObj* gobj, HSD_GObjEvent vanilla)
{
    Fighter* fp = GET_FIGHTER(gobj);
    if (ucf_ShieldDropVeto(fp)) {
        ftCommonData* cd = p_ftCommonData;
        f32 saved = cd->x314;
        cd->x314 = -2.0f;
        vanilla(gobj);
        cd->x314 = saved;
    } else {
        vanilla(gobj);
    }
}

static void ucf_GuardOn_IASA(HSD_GObj* gobj)
{
    ucf_DropGuardedIASA(gobj, (HSD_GObjEvent) ftCo_GuardOn_IASA);
}
static void ucf_Guard_IASA(HSD_GObj* gobj)
{
    ucf_DropGuardedIASA(gobj, (HSD_GObjEvent) ftCo_Guard_IASA);
}
static void ucf_GuardOff_IASA(HSD_GObj* gobj)
{
    ucf_DropGuardedIASA(gobj, (HSD_GObjEvent) ftCo_GuardOff_IASA);
}
static void ucf_GuardReflect_IASA(HSD_GObj* gobj)
{
    ucf_DropGuardedIASA(gobj, (HSD_GObjEvent) ftCo_GuardReflect_IASA);
}
static void ucf_Wait_IASA(HSD_GObj* gobj)
{
    ucf_DropGuardedIASA(gobj, (HSD_GObjEvent) ftCo_Wait_IASA);
}
static void ucf_AppealS_IASA(HSD_GObj* gobj)
{
    ucf_DropGuardedIASA(gobj, (HSD_GObjEvent) ftCo_AppealS_IASA);
}

/* ---- install -------------------------------------------------------------- */

void lbUcf_Install(void)
{
    static bool installed = false;
    if (installed) {
        return;
    }
    installed = true;

    ftData_MotionStateList[ftCo_MS_Turn].input_cb = ucf_Turn_IASA;
    ftData_MotionStateList[ftCo_MS_DamageFall].input_cb = ucf_DamageFall_IASA;
    /* every table-dispatched state that can enter the shield drop */
    ftData_MotionStateList[ftCo_MS_GuardOn].input_cb = ucf_GuardOn_IASA;
    ftData_MotionStateList[ftCo_MS_Guard].input_cb = ucf_Guard_IASA;
    ftData_MotionStateList[ftCo_MS_GuardOff].input_cb = ucf_GuardOff_IASA;
    ftData_MotionStateList[ftCo_MS_GuardReflect].input_cb =
        ucf_GuardReflect_IASA;
    ftData_MotionStateList[ftCo_MS_Wait].input_cb = ucf_Wait_IASA;
    ftData_MotionStateList[ftCo_MS_AppealSL].input_cb = ucf_AppealS_IASA;
    ftData_MotionStateList[ftCo_MS_AppealSR].input_cb = ucf_AppealS_IASA;
}
