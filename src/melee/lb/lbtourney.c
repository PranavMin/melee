#include "lbtourney.h"

#include <string.h>

#include <dolphin/pad.h>
#include <melee/ft/forward.h>
#include <melee/gm/gmvsmelee.h>
#include <melee/lb/lbrelayexi.h>
#include <melee/mn/mncharsel.h>
#include <melee/mn/types.h>
#include <melee/pl/forward.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/sislib.h>

#define LB_TOURNEY_TIMEOUT_FRAMES (5 * 60)
#define LB_TOURNEY_END_HOLD_FRAMES 60

static struct set_entry cur_set;
static bool has_set = false;
static struct game_result games[MAX_GAMES];
static u8 game_count;
static u8 pending_cmd; /* 0 = idle, else the relay_cmd in flight */
static bool last_failed;
static u32 timeout;
static u32 end_hold; /* consecutive frames Z + D-up has been held */

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

/* Slot 1 / slot 2 external character ids from the CSS selection state: the
 * first two human ports, ChKind_None when there is no such port. */
static void readChars(u8* p1, u8* p2)
{
    int port;
    int found = 0;
    *p1 = ChKind_None;
    *p2 = ChKind_None;
    for (port = 0; port < GM_MAX_PLAYERS && found < 2; port++) {
        const PlayerInitData* pl = &gmVsMelee_CssData.vs.start.players[port];
        if (pl->slot_type == Gm_PKind_Human) {
            if (found == 0) {
                *p1 = (u8) pl->ckind;
            } else {
                *p2 = (u8) pl->ckind;
            }
            found++;
        }
    }
}

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
    readChars(&game->p1_char, &game->p2_char);
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

static void handleInputs(void)
{
    int port;
    u32 dpad_pressed = 0; /* d-pad triggers on ports currently holding Z */
    bool z_up_held = false;

    for (port = 0; port < 4; port++) {
        const HSD_PadStatus* pad = &HSD_PadCopyStatus[port];
        if (!(pad->button & PAD_TRIGGER_Z)) {
            continue;
        }
        dpad_pressed |= pad->trigger &
                        (PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT | PAD_BUTTON_DOWN);
        /* End set = Z + D-up held. D-up (not Start) because Start is Melee's
         * native "advance to stage select" on the CSS, which fires before any
         * hold can complete and leaves the screen. D-up has no native CSS use
         * and is the one d-pad direction the score binds don't consume. */
        if (pad->button & PAD_BUTTON_UP) {
            z_up_held = true;
        }
    }

    if (z_up_held) {
        if (++end_hold == LB_TOURNEY_END_HOLD_FRAMES) {
            int need = cur_set.best_of / 2 + 1;
            /* END_SET needs a decided score; ignore the hold otherwise. */
            if (winsFor(1) >= need || winsFor(2) >= need) {
                sendEndSet();
            }
        }
        return; /* don't also count a d-pad press in the same chord */
    }
    end_hold = 0;

    if (dpad_pressed & PAD_BUTTON_LEFT) {
        appendGame(1);
    } else if (dpad_pressed & PAD_BUTTON_RIGHT) {
        appendGame(2);
    } else if (dpad_pressed & PAD_BUTTON_DOWN) {
        undoGame();
    }
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
            /* Set reported and closed; the overlay disappears. */
            has_set = false;
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
    entry = HSD_SisLib_803A6B98(css_text, 24.0f, 24.0f, "%s %d - %d %s%s", p1,
                                winsFor(1), winsFor(2), p2,
                                pending_cmd != 0 ? " !"
                                : last_failed    ? " X"
                                                 : "");
    HSD_SisLib_803A7548(css_text, entry, 0.55f, 0.55f);
}

void lbTourney_CSSFrame(void)
{
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
