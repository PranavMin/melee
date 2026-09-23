#include "lbtourney.h"

#include <math.h>
#include <string.h>

#include <dolphin/pad.h>
#include <melee/ft/forward.h>
#include <melee/gm/forward.h>
#include <melee/gm/gm_1601.h>
#include <melee/gm/gm_1A3F.h>
#include <melee/gm/gmmain_lib.h>
#include <melee/gm/gmscene.h>
#include <melee/gm/gmvs.h>
#include <melee/gm/gmvsmelee.h>
#include <melee/gm/types.h>
#include <melee/if/iftime.h>
#include <melee/lb/lbbuttonglyph.h>
#include <melee/lb/lbrelayexi.h>
#include <melee/lb/lbucf.h>
#include <melee/mn/mntourney.h>
#include <melee/mn/mncharsel.h>
#include <melee/mn/mnname.h>
#include <melee/mn/types.h>
#include <melee/pl/forward.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/rumble.h>
#include <sysdolphin/baselib/sislib.h>

/* The vibration menu's confirmation-pulse rumble list (mnvibration.c, global
 * but not exported through its header). The venue's D-pad rumble hook passes
 * exactly this list to HSD_PadRumbleAdd. */
extern s32 mnVibration_804D4FF0;

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

/* Handwarmer flag (user, 2026-09-22): the next game is a warm-up that does
 * not count toward the score. Toggled on the CSS with Z + X (any port),
 * shown on the CSS overlay, drawn in-match as a count-up clock that turns red
 * past 1:00 (design.md sec 12), and cleared automatically once that game has
 * been played, so it can never linger into a real game. The C-stick score
 * binds are unaffected: they stay the one source of score truth. */
#define LB_TOURNEY_HANDWARMER_RED_FRAMES (60 * 60) /* 1:00 */
static bool handwarmer;
static bool stage_sel_forced; /* rules->stage_sel is Random for this start */

/* CSS overlay layout. The values are the shipped positions (tuned live by
 * the user, 2026-09-22). Set LB_TOURNEY_LAYOUT_TUNE to 1 to compile the live
 * layout tune mode back in (see tuneInputs): hold L + R on the CSS, D-pad
 * moves the selected element by 2 px (10 with Z), X selects the next element,
 * Y grows the scale (shrinks with Z), and a readout "TUNE SCORE X 212 Y 4
 * S 52" shows the live numbers for ~3 s. Read them off the screen and
 * hardcode them here, then set the define back to 0 for a venue build. */
#define LB_TOURNEY_LAYOUT_TUNE 0
enum lbTourney_El { EL_SCORE, EL_HINT, EL_STATUS, EL_COUNT };
static struct {
    f32 x, y, scale;
} el[EL_COUNT] = {
    { 188.0f, -4.0f, 0.62f },  /* score, top centre (tuned live, 2026-09-22) */
    { 456.0f, 446.0f, 0.43f }, /* handwarmer hint, bottom right (tuned live) */
    { 2.0f, 446.0f, 0.45f },   /* SENDING / SENT / FAILED, bottom left (tuned
                                * live) */
};
#if LB_TOURNEY_LAYOUT_TUNE
static const char* const el_names[EL_COUNT] = { "SCORE", "HINT", "STATUS" };
static int tune_sel;
static u32 tune_show; /* frames left showing the tune readout */
#endif
static bool match_seen;   /* a GS_VS frame ran since the last CSS frame */
static u32 match_frames;  /* frames since the match scene began */
static s32 vs_ctx = -1;   /* in-match SIS overlay, per GS_VS visit */
static HSD_Text* vs_text = NULL;
static int vs_shown_sec = -1;

/* Nametag seeding: the set's two tags are written into persistent nametag
 * slots 0 and 1 (the top of the CSS tag dropdown), so each player picks
 * their own tag and the kiosk knows which entrant is on which port. Melee
 * tags are four characters, upper-case A-Z and digits only; anything else
 * in a start.gg tag is dropped, and an empty result falls back to P1/P2. The
 * kiosk memory card is the venue's, so overwriting its first two tags is
 * fine (any other tags on it stay). */
#define LB_TOURNEY_TAG_CHARS 4

static void writeNametag(int slot, const char* tag, const char* fallback)
{
    struct NameTagData* nd;
    char buf[LB_TOURNEY_TAG_CHARS + 1];
    int i;
    int n = 0;

    for (i = 0; i < TAG_LEN && tag[i] != '\0' && n < LB_TOURNEY_TAG_CHARS;
         i++)
    {
        char c = tag[i];
        if (c >= 'a' && c <= 'z') {
            c -= 'a' - 'A';
        }
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            buf[n++] = c;
        }
    }
    if (n == 0) {
        while (fallback[n] != '\0') {
            buf[n] = fallback[n];
            n++;
        }
    }
    buf[n] = '\0';
    /* Marks the slot empty and resets its KO/stat records; namedata is then
     * a plain NUL-terminated SIS string, as the default-name init writes. */
    CreateNameAtIndex(slot);
    nd = GetPersistentNameData(slot);
    memcpy(nd->namedata, buf, n + 1);
    /* A tagged player's in-match rumble comes from the TAG's flag, not the
     * port's (gm_RumbleEnabledForPlayer). Start off, like the venue default;
     * the D-pad toggle and tag pick keep it in step with the port below. */
    nd->rumble_enabled = false;
}

/* Effective rumble for a port on this CSS: the port pref, or the picked
 * tag's flag once a tag is picked (what the match will actually use). */
static bool portRumble(int port)
{
    return gm_RumbleEnabledForPlayer(port, mnCharSel_PortNametag(port));
}

static void setPortRumble(int port, bool on)
{
    int slot = mnCharSel_PortNametag(port);
    gmMainLib_SetRumbleEnabled(port, on);
    if (slot != GM_NAMETAG_COUNT) {
        GetPersistentNameData(slot)->rumble_enabled = on;
    }
}

/* The CSS port (0-3) that picked nametag `slot`, or -1. */
static int portWithTag(int slot)
{
    int port;
    for (port = 0; port < 4; port++) {
        if (mnCharSel_PortNametag(port) == slot) {
            return port;
        }
    }
    return -1;
}

/* Selection-hand shake: the venue's D-pad rumble hook shoves the port's CSS
 * cursor (CSSCursorData xC, its live X) left by 3 and springs it back so the
 * hand visibly "rumbles" on a toggle. Its spring constants live in unnamed
 * .sdata2 floats that did not resolve, and as decoded its reversal test reads
 * the absolute X, so the visible effect is reproduced instead: a decaying
 * horizontal oscillation of amplitude 3, applied as per-frame deltas to xC.
 * State is ours (the asm parks its velocity past the end of the struct). This
 * runs in the scene on_frame, before HSD_GObj_RunProcs, so the delta lands
 * before CursorThink adds stick input and the render reads xC. */
#define HAND_KICK_AMP 3.0f  /* the asm's initial velocity magnitude */
#define HAND_STEP 0.8f      /* radians per frame: ~4-frame half-period */
#define HAND_DECAY 0.82f    /* amplitude kept per frame: ~17 visible frames */
#define HAND_DONE 0.1f      /* amplitude below this = settled */
static f32 hand_amp[4];   /* 0 = idle */
static f32 hand_phase[4];
static f32 hand_pos[4];   /* displacement currently applied to xC */

static void kickHand(int port)
{
    hand_amp[port] = HAND_KICK_AMP;
    hand_phase[port] = 0.0f;
}

static void stepHand(int port)
{
    f32* x;
    f32 pos;
    if (hand_amp[port] == 0.0f) {
        return;
    }
    x = mnCharSel_CursorHandOffset(port);
    /* pos = -amp*cos(phase): starts kicked left, swings back, decays */
    pos = -hand_amp[port] * cosf(hand_phase[port]);
    if (x != NULL) {
        *x += pos - hand_pos[port];
    }
    hand_pos[port] = pos;
    hand_phase[port] += HAND_STEP;
    hand_amp[port] *= HAND_DECAY;
    if (hand_amp[port] < HAND_DONE) {
        if (x != NULL) {
            *x -= hand_pos[port]; /* leave the hand exactly where it was */
        }
        hand_amp[port] = 0.0f;
        hand_pos[port] = 0.0f;
    }
}

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
    handwarmer = false;
    match_seen = false;
    has_set = true;
    css_dirty = true;
    writeNametag(0, cur_set.p1_tag, "P1");
    writeNametag(1, cur_set.p2_tag, "P2");
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
        if (pad->trigger & PAD_BUTTON_X) {
            /* Z + X: straight into a handwarmer (user, 2026-09-22). When the
             * CSS is ready the fight starts now on a random legal stage, no
             * stage select: the game rules' stage_sel is flipped to Random
             * for this one transition (the CSS-exit code then fills
             * force_stage_id from mnSelStageRandom(), which draws from the
             * kiosk's six, and the SSS skips itself on its first frame);
             * lbTourney_MatchFrame puts it back. When the CSS is not ready
             * (someone has not picked) the press just arms/disarms the flag. */
            if (mnCharSel_TryStartFight()) {
                handwarmer = true;
                gmMainLib_GetGameRules()->stage_sel = 1; /* random */
                stage_sel_forced = true;
            } else {
                handwarmer = !handwarmer;
            }
            css_dirty = true;
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

/* "P3" for the port that picked nametag `slot`, "" if nobody has yet. */
static const char* portLabel(int slot)
{
    static const char* const labels[4] = { "P1", "P2", "P3", "P4" };
    int port = portWithTag(slot);
    return port < 0 ? "" : labels[port];
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

    /* Handwarmer hint (bottom right) with button icons: the bind when off,
     * the armed state when on. The user placed the plain "ZX FOR HANDWARMER"
     * at el[EL_HINT].x; that text's right edge is kept as the anchor and the
     * icon versions are right-aligned to it. */
    {
        f32 s = el[EL_HINT].scale;
        f32 right = el[EL_HINT].x + lbButton_Measure(s, "ZX FOR HANDWARMER");
        const char* fmt =
            handwarmer ? "#Z+#X CANCELS HANDWARMER" : "#Z+#X FOR HANDWARMER";
        lbButton_Line(css_text, right - lbButton_Measure(s, fmt),
                      el[EL_HINT].y, s, fmt);
    }

    /* Score (top centre). Each name carries the port that picked its nametag
     * (who is who). SIS y draws ~12 px lower than given; the screen is cut at
     * ~470 in Dolphin, earlier on a CRT. */
    entry = HSD_SisLib_803A6B98(css_text, el[EL_SCORE].x, el[EL_SCORE].y,
                                "%s %s  %d - %d  %s %s", p1, portLabel(0),
                                winsFor(1), winsFor(2), portLabel(1), p2);
    HSD_SisLib_803A7548(css_text, entry, el[EL_SCORE].scale,
                        el[EL_SCORE].scale);

    /* Status (bottom left): in-flight, just-sent, or failed. (No "NEXT:
     * GAME n" line: that belongs with automatic winner detection, design.md
     * sec 12.) */
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
        entry = HSD_SisLib_803A6B98(css_text, el[EL_STATUS].x, el[EL_STATUS].y,
                                    "%s", status);
        HSD_SisLib_803A7548(css_text, entry, el[EL_STATUS].scale,
                            el[EL_STATUS].scale);
    }

#if LB_TOURNEY_LAYOUT_TUNE
    /* Layout tune readout, mid-screen, while tuning. Scale shown x100. */
    if (tune_show > 0) {
        entry = HSD_SisLib_803A6B98(css_text, 120.0f, 200.0f,
                                    "TUNE %s  X %d  Y %d  S %d",
                                    el_names[tune_sel], (int) el[tune_sel].x,
                                    (int) el[tune_sel].y,
                                    (int) (el[tune_sel].scale * 100.0f + 0.5f));
        HSD_SisLib_803A7548(css_text, entry, 0.5f, 0.5f);
    }
#endif
}

#if LB_TOURNEY_LAYOUT_TUNE
/* Layout tune mode (see the el[] comment). Returns true while any port holds
 * L + R, during which that chord owns the D-pad (so the rumble toggle must
 * not also fire). */
static bool tuneInputs(void)
{
    int port;
    bool tuning = false;
    for (port = 0; port < 4; port++) {
        const HSD_PadStatus* pad = &HSD_PadCopyStatus[port];
        u32 trig = pad->trigger;
        f32 step;
        if ((pad->button & (PAD_TRIGGER_L | PAD_TRIGGER_R)) !=
            (PAD_TRIGGER_L | PAD_TRIGGER_R))
        {
            continue;
        }
        tuning = true;
        step = (pad->button & PAD_TRIGGER_Z) ? 10.0f : 2.0f;
        if (trig & PAD_BUTTON_LEFT) {
            el[tune_sel].x -= step;
        } else if (trig & PAD_BUTTON_RIGHT) {
            el[tune_sel].x += step;
        } else if (trig & PAD_BUTTON_UP) {
            el[tune_sel].y -= step;
        } else if (trig & PAD_BUTTON_DOWN) {
            el[tune_sel].y += step;
        } else if (trig & PAD_BUTTON_X) {
            tune_sel = (tune_sel + 1) % EL_COUNT;
        } else if (trig & PAD_BUTTON_Y) {
            el[tune_sel].scale +=
                (pad->button & PAD_TRIGGER_Z) ? -0.02f : 0.02f;
            if (el[tune_sel].scale < 0.1f) {
                el[tune_sel].scale = 0.1f;
            }
        } else {
            continue;
        }
        tune_show = 180;
        css_dirty = true;
    }
    return tuning;
}
#else
static bool tuneInputs(void)
{
    return false;
}
#endif

/* In-match handwarmer clock: "HANDWARMER 0:42" in the top-left corner (the
 * HUD timer owns top-centre), counting up from the scene start, red once it
 * passes 1:00. Redrawn once a second. */
static void redrawMatch(void)
{
    static GXColor red = { 255, 64, 64, 255 };
    int sec = match_frames / 60;
    int entry;

    if (vs_text != NULL) {
        HSD_SisLib_803A5CC4(vs_text);
        vs_text = NULL;
    }
    vs_text = HSD_SisLib_803A6754(0, vs_ctx);
    vs_text->default_kerning = 1;
    entry = HSD_SisLib_803A6B98(vs_text, 28.0f, 26.0f, "HANDWARMER  %d:%02d",
                                sec / 60, sec % 60);
    HSD_SisLib_803A7548(vs_text, entry, 0.6f, 0.6f);
    if (match_frames >= LB_TOURNEY_HANDWARMER_RED_FRAMES) {
        HSD_SisLib_803A74F0(vs_text, entry, &red);
    }
    vs_shown_sec = sec;
}

void lbTourney_MatchFrame(void)
{
    if (stage_sel_forced) {
        /* The CSS-exit code has consumed it; the next set-up chooses again. */
        gmMainLib_GetGameRules()->stage_sel = 0; /* choose */
        stage_sel_forced = false;
    }
    if (has_set) {
        if (!match_seen) {
            match_seen = true;
            match_frames = 0;
        } else {
            match_frames++;
        }
        if (handwarmer) {
            if (vs_ctx < 0) {
                vs_ctx = HSD_SisLib_803A611C(0, NULL, 9, 0xD, 0, 0xE, 0, 0x13);
                vs_shown_sec = -1;
            }
            if ((int) (match_frames / 60) != vs_shown_sec) {
                redrawMatch();
            }
        }
    }
    gm_Scene_Vs_OnFrame();
    if (has_set && handwarmer) {
        /* One clock only: the HUD's countdown is hidden for a handwarmer
         * (user, 2026-09-22). Done after the vanilla frame and re-asserted
         * every frame because the HUD shows its timers again on its own
         * (gmvs.c:1135, ifall.c:51 - pause/HUD toggles); the flag is
         * idempotent and the timer itself keeps running underneath. */
        ifTime_HideTimers();
    }
}

void lbTourney_MatchExit(void* arg)
{
    /* The scene teardown frees the canvas and text GObjs; just forget them. */
    vs_ctx = -1;
    vs_text = NULL;
    vs_shown_sec = -1;
    gm_Scene_Vs_OnExit(arg);
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
    /* Venue D-pad rumble: native port of the Tournament mods' CSS hook
     * (g_mods_tournament.bin code at mnCharSel_CursorThink+0x638). On the CSS,
     * D-pad UP turns a port's rumble on, with the same confirmation pulse the
     * vibration menu plays; D-pad DOWN turns it off; each only when the
     * setting actually changes. The asm also bounces the CSS rumble icon (a
     * cosmetic -3.0/-2.5 spring) - not carried. Edge-triggered here, which
     * stands in for the asm's per-port "handled" latch. */
    {
        int port;
        bool tuning = has_set && tuneInputs();
        for (port = 0; port < 4; port++) {
            u32 trig = HSD_PadCopyStatus[port].trigger;
            if (tuning) {
                trig = 0; /* the L+R chord owns the D-pad this frame */
            }
            if ((trig & PAD_BUTTON_UP) && !portRumble(port)) {
                setPortRumble(port, true);
                HSD_PadRumbleAdd(port, 0, 14, 0, &mnVibration_804D4FF0);
                kickHand(port); /* the asm bounces the hand on any toggle */
            } else if ((trig & PAD_BUTTON_DOWN) && portRumble(port)) {
                setPortRumble(port, false);
                kickHand(port);
            }
            stepHand(port);
        }
    }
    if (has_set) {
        if (css_ctx < 0) {
            css_ctx = HSD_SisLib_803A611C(0, NULL, 9, 0xD, 0, 0xE, 0, 0x13);
            css_dirty = true;
        }
        if (match_seen) {
            /* Back from a game: a handwarmer flag covered exactly that game. */
            match_seen = false;
            handwarmer = false;
            css_dirty = true;
        }
        /* The port <-> nametag pairing changes as players pick tags. */
        {
            static int shown_p1_port = -2, shown_p2_port = -2;
            int a = portWithTag(0), b = portWithTag(1);
            if (a != shown_p1_port || b != shown_p2_port) {
                /* A newly picked tag inherits the port's rumble choice, so
                 * a D-pad toggle made before picking still holds. */
                if (a >= 0 && a != shown_p1_port) {
                    GetPersistentNameData(0)->rumble_enabled =
                        GetRumbleSettingOfPort(a);
                }
                if (b >= 0 && b != shown_p2_port) {
                    GetPersistentNameData(1)->rumble_enabled =
                        GetRumbleSettingOfPort(b);
                }
                shown_p1_port = a;
                shown_p2_port = b;
                css_dirty = true;
            }
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
#if LB_TOURNEY_LAYOUT_TUNE
        if (tune_show > 0 && --tune_show == 0) {
            css_dirty = true; /* drop the tune readout */
        }
#endif
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
