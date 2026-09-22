#include "lbtourney.h"

#include <string.h>

#include <dolphin/pad.h>
#include <melee/ft/forward.h>
#include <melee/gm/forward.h>
#include <melee/gm/gm_1A3F.h>
#include <melee/gm/gmmain_lib.h>
#include <melee/gm/gmscene.h>
#include <melee/gm/gmvsmelee.h>
#include <melee/gm/types.h>
#include <melee/lb/lbrelayexi.h>
#include <melee/lb/lbucf.h>
#include <melee/mn/mntourney.h>
#include <melee/mn/mncharsel.h>
#include <melee/mn/types.h>
#include <melee/pl/forward.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/sislib.h>

#define LB_TOURNEY_TIMEOUT_FRAMES (5 * 60)
#define LB_TOURNEY_END_HOLD_FRAMES 60
#define LB_TOURNEY_SENT_FLASH_FRAMES 75 /* ~1.25 s of "SENT" after a report */
/* C-stick deflection past this (raw, full throw ~80) counts as a direction.
 * The C-stick, not the d-pad: it has no native CSS action and is reachable on
 * standard controllers and box controllers alike (d-pad is not). */
#define LB_TOURNEY_CSTICK_THRESH 56

enum lbTourney_CDir {
    CDIR_NONE = 0,
    CDIR_LEFT,
    CDIR_RIGHT,
    CDIR_UP,
    CDIR_DOWN,
};

static struct set_entry cur_set;
static bool has_set = false;
static struct game_result games[MAX_GAMES];
static u8 game_count;
static u8 pending_cmd; /* 0 = idle, else the relay_cmd in flight */
static bool last_failed;
static u32 timeout;
static u32 end_hold; /* consecutive frames Z + C-up has been held */
static int prev_cdir; /* last frame's aggregate C-stick direction, for edges */
static u32 sent_flash; /* frames left showing the "SENT" confirmation */

/* Score overlay, screen-space SIS canvas like the title-screen timestamp.
 * Recreated per CSS visit; the scene teardown frees the objects and
 * lbTourney_CSSExit forgets them. */
static s32 css_ctx = -1;
static HSD_Text* css_text = NULL;
static bool css_dirty;

void lbTourney_SetCurrent(const struct set_entry* set)
{
    cur_set = *set;
    memset(games, 0, sizeof(games));
    game_count = 0;
    pending_cmd = 0;
    last_failed = false;
    end_hold = 0;
    prev_cdir = CDIR_NONE;
    sent_flash = 0;
    has_set = true;
    css_dirty = true;
}

void lbTourney_ClearCurrent(void)
{
    has_set = false;
    pending_cmd = 0;
    css_dirty = true;
}

bool lbTourney_HasCurrent(void)
{
    return has_set;
}

static int winsFor(int slot)
{
    int i;
    int n = 0;
    for (i = 0; i < game_count; i++) {
        if (games[i].winner_slot == slot) {
            n++;
        }
    }
    return n;
}

/* v1 reports winners only; per-game character data is deferred to the v2
 * GAME_END hook (design.md R13). Reading characters from CSS port order was
 * unreliable: port order need not match entrant order, and a character may
 * not be locked in when the game is reported. So games carry no character. */

static void sendReport(void)
{
    struct report_score_req req;
    memset(&req, 0, sizeof(req));
    req.set_id = cur_set.set_id;
    req.game_count = game_count;
    memcpy(req.games, games, sizeof(req.games));
    if (lbRelayExi_Request(CMD_REPORT_SCORE, &req, sizeof(req))) {
        pending_cmd = CMD_REPORT_SCORE;
        timeout = 0;
        last_failed = false;
    } else {
        last_failed = true;
    }
    css_dirty = true;
}

static void sendEndSet(void)
{
    struct end_set_req req;
    memset(&req, 0, sizeof(req));
    req.set_id = cur_set.set_id;
    req.game_count = game_count;
    memcpy(req.games, games, sizeof(req.games));
    if (lbRelayExi_Request(CMD_END_SET, &req, sizeof(req))) {
        pending_cmd = CMD_END_SET;
        timeout = 0;
        last_failed = false;
    } else {
        last_failed = true;
    }
    css_dirty = true;
}

static void appendGame(int winner_slot)
{
    struct game_result* game;
    if (game_count >= MAX_GAMES) {
        return;
    }
    game = &games[game_count];
    game->winner_slot = winner_slot;
    game->p1_char = 0; /* winners only in v1; relay ignores these (R13) */
    game->p2_char = 0;
    game->_pad = 0;
    game_count++;
    sendReport();
}

static void undoGame(void)
{
    if (game_count == 0) {
        return;
    }
    game_count--;
    memset(&games[game_count], 0, sizeof(games[game_count]));
    sendReport();
}

/* Cardinal C-stick direction for one pad, or CDIR_NONE when centred. The
 * dominant axis wins so diagonals resolve cleanly. subStickY is +up. */
static int cstickDir(const HSD_PadStatus* pad)
{
    s32 x = pad->subStickX;
    s32 y = pad->subStickY;
    s32 ax = x < 0 ? -x : x;
    s32 ay = y < 0 ? -y : y;

    if (ax < LB_TOURNEY_CSTICK_THRESH && ay < LB_TOURNEY_CSTICK_THRESH) {
        return CDIR_NONE;
    }
    if (ax >= ay) {
        return x < 0 ? CDIR_LEFT : CDIR_RIGHT;
    }
    return y > 0 ? CDIR_UP : CDIR_DOWN;
}

static void handleInputs(void)
{
    int port;
    int cdir = CDIR_NONE; /* first non-centred C-stick among Z-held ports */

    /* Any controller may drive the set: all four ports are read, and any port
     * holding Z counts. (In Dolphin only port 1 is emulated by default.) */
    for (port = 0; port < 4; port++) {
        const HSD_PadStatus* pad = &HSD_PadCopyStatus[port];
        if (!(pad->button & PAD_TRIGGER_Z)) {
            continue;
        }
        if (cdir == CDIR_NONE) {
            cdir = cstickDir(pad);
        }
    }

    /* End set = Z + C-up held ~1 s (sustained, not a flick). C-stick, not
     * Start: Start is Melee's native "advance to stage select" on the CSS. */
    if (cdir == CDIR_UP) {
        if (++end_hold == LB_TOURNEY_END_HOLD_FRAMES) {
            int need = cur_set.best_of / 2 + 1;
            /* END_SET needs a decided score; ignore the hold otherwise. */
            if (winsFor(1) >= need || winsFor(2) >= need) {
                sendEndSet();
            }
        }
        prev_cdir = cdir;
        return;
    }
    end_hold = 0;

    /* Score / undo fire once per flick: on the edge into a new direction. */
    if (cdir != prev_cdir) {
        if (cdir == CDIR_LEFT) {
            appendGame(1);
        } else if (cdir == CDIR_RIGHT) {
            appendGame(2);
        } else if (cdir == CDIR_DOWN) {
            undoGame();
        }
    }
    prev_cdir = cdir;
}

static void pollRelay(void)
{
    const struct lbRelayExi_PollBuf* r;
    s32 state = lbRelayExi_Poll();

    if (state < 0 || state == RELAY_ERROR) {
        pending_cmd = 0;
        last_failed = true;
        css_dirty = true;
        return;
    }
    if (state != RELAY_DONE) {
        if (++timeout > LB_TOURNEY_TIMEOUT_FRAMES) {
            lbRelayExi_Abort();
            pending_cmd = 0;
            last_failed = true;
            css_dirty = true;
        }
        return;
    }
    r = lbRelayExi_Response();
    if (r->hdr.magic[0] == RELAY_MAGIC_0 && r->hdr.magic[1] == RELAY_MAGIC_1 &&
        r->hdr.cmd == pending_cmd && r->resp.status == ST_OK)
    {
        if (pending_cmd == CMD_END_SET) {
            /* Set reported and closed: leave the CSS back to the set list.
             * force_main_menu makes GM_MENU's onEnter land on our menu; the
             * transition pair is the one vanilla CSS-back uses. */
            has_set = false;
            gmMainLib_GetGameRules()->force_main_menu = 1;
            mnTourney_ArmAutoEnter();
            gm_ChangeGameModeAfterCurrentScene(GM_MENU);
            gm_801A4B60();
        } else if (pending_cmd == CMD_REPORT_SCORE) {
            sent_flash = LB_TOURNEY_SENT_FLASH_FRAMES;
        }
        last_failed = false;
    } else {
        last_failed = true;
    }
    pending_cmd = 0;
    css_dirty = true;
}

static void redraw(void)
{
    char p1[TAG_LEN + 1];
    char p2[TAG_LEN + 1];
    int entry;
    const char* status;

    if (css_text != NULL) {
        HSD_SisLib_803A5CC4(css_text);
        css_text = NULL;
    }
    if (!has_set) {
        return;
    }
    memcpy(p1, cur_set.p1_tag, TAG_LEN);
    p1[TAG_LEN] = '\0';
    memcpy(p2, cur_set.p2_tag, TAG_LEN);
    p2[TAG_LEN] = '\0';

    css_text = HSD_SisLib_803A6754(0, css_ctx);
    css_text->default_kerning = 1;

    /* Score at the bottom, under the character panels, clear of the top HUD. */
    entry = HSD_SisLib_803A6B98(css_text, 190.0f, 442.0f, "%s  %d - %d  %s", p1,
                                winsFor(1), winsFor(2), p2);
    HSD_SisLib_803A7548(css_text, entry, 0.7f, 0.7f);

    /* Status just above the score: in-flight, just-sent, or failed. */
    if (pending_cmd != 0) {
        status = "SENDING...";
    } else if (last_failed) {
        status = "SEND FAILED";
    } else if (sent_flash > 0) {
        status = "SCORE SENT";
    } else {
        status = NULL;
    }
    if (status != NULL) {
        entry = HSD_SisLib_803A6B98(css_text, 190.0f, 416.0f, "%s", status);
        HSD_SisLib_803A7548(css_text, entry, 0.55f, 0.55f);
    }
}

void lbTourney_CSSFrame(void)
{
    /* Kiosk: every CSS visit comes from our menu (a tournament set or
     * friendlies), so any B-back returns to the set list, not the VS menu.
     * Keep force_main_menu set so when vanilla's B-back leaves the CSS,
     * GM_MENU's onEnter lands on our Tournament menu. */
    gmMainLib_GetGameRules()->force_main_menu = 1;
    mnTourney_ArmAutoEnter();
    /* Venue UCF 0.8: one-time swap of IASA pointers in ftData_MotionStateList
     * (guarded, idempotent). Every kiosk match is entered from this CSS, and
     * the table is consulted every match frame, so installing here is in time
     * for all of them. No matched-function edit. */
    lbUcf_Install();
    if (has_set) {
        if (css_ctx < 0) {
            css_ctx = HSD_SisLib_803A611C(0, NULL, 9, 0xD, 0, 0xE, 0, 0x13);
            css_dirty = true;
        }
        if (pending_cmd != 0) {
            /* Ignore inputs while a request is in flight. */
            end_hold = 0;
            pollRelay();
        } else {
            handleInputs();
        }
        if (sent_flash > 0) {
            sent_flash--;
            css_dirty = true; /* keep "SCORE SENT" up, then clear it */
        }
        if (css_dirty) {
            css_dirty = false;
            redraw();
        }
    }
    mnCharSel_Scene_OnFrame();
}

void lbTourney_CSSExit(void* arg)
{
    /* The scene teardown frees the canvas and text GObjs; just forget
     * them. */
    css_ctx = -1;
    css_text = NULL;
    mnCharSel_Scene_OnExit(arg);
}
