#include "lbtourney.h"

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
#include <melee/mn/mntourney.h>
#include <melee/mn/mncharsel.h>
#include <melee/mn/mnname.h>
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

/* Handwarmer flag (user, 2026-09-22): the next game is a warm-up that does
 * not count toward the score. Toggled on the CSS with Z + X (any port),
 * shown on the CSS overlay, drawn in-match as a count-up clock that turns red
 * past 1:00 (design.md sec 12), and cleared automatically once that game has
 * been played, so it can never linger into a real game. The C-stick score
 * binds are unaffected: they stay the one source of score truth. */
#define LB_TOURNEY_HANDWARMER_RED_FRAMES (60 * 60) /* 1:00 */
static bool handwarmer;
static bool stage_sel_forced; /* rules->stage_sel is Random for this start */

/* Auto-score state (see autoScoreFromMatch below). */
static int auto_pending;      /* entrant (1/2) who won the game just played */
static u8 auto_chars[2];      /* external CharacterKind of entrant 1 and 2 */
static u8 auto_stage;         /* internal StKind the game was played on */
static char auto_note[40];    /* why nothing was scored, or what was */
static u32 auto_note_frames;  /* frames left showing auto_note */

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
     * lbTourney_CSSFrame mirrors the port's pref into it every frame. */
    nd->rumble_enabled = false;
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

/* Who is who. Nametag slot 0 is entrant 1, slot 1 is entrant 2 (seeded at
 * START_SET). Rule (user, 2026-09-22): when exactly two people are playing
 * and only one has picked a tag, the other player is the other entrant.
 * tags[i] is candidate i's nametag slot; out[i] gets 1, 2 or 0 (unknown). */
static void assignEntrants(const u8* tags, int n, u8* out)
{
    int i;
    int known = 0;
    int unknown_i = -1;
    for (i = 0; i < n; i++) {
        out[i] = tags[i] == 0 ? 1 : tags[i] == 1 ? 2 : 0;
        if (out[i] != 0) {
            known++;
        } else {
            unknown_i = i;
        }
    }
    if (n == 2 && known == 1) {
        out[unknown_i] = 3 - out[1 - unknown_i];
    }
}

/* The CSS port playing as `entrant` (1 or 2) by the rule above, or -1. Only
 * human doors count; the tags of CPU doors cannot be picked anyway. */
static int entrantPort(int entrant)
{
    u8 tags[4];
    u8 who[4];
    int ports[4];
    int n = 0;
    int port;
    for (port = 0; port < 4; port++) {
        if (mnCharSel_PortSlotType(port) == Gm_PKind_Human) {
            ports[n] = port;
            tags[n] = mnCharSel_PortNametag(port);
            n++;
        }
    }
    assignEntrants(tags, n, who);
    for (port = 0; port < n; port++) {
        if (who[port] == entrant) {
            return ports[port];
        }
    }
    return -1;
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
    auto_pending = 0;
    auto_note_frames = 0;
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

/* Appends a game. Characters are the entrants' external CharacterKind ids
 * (0 = Captain Falcon, so "unknown" is CHAR_UNKNOWN = 0xFF) and stage the
 * internal StKind (0 = unknown). A game scored by hand sends unknowns: the CSS
 * cannot tell which entrant sat where. The auto-score path fills them from
 * the match standings, where they are authoritative (design.md R13); the
 * relay omits anything it cannot map rather than rejecting the report. */
#define CHAR_UNKNOWN 0xFF
static void appendGame(int winner_slot, u8 p1_char, u8 p2_char, u8 stage)
{
    struct game_result* game;
    if (game_count >= MAX_GAMES) {
        return;
    }
    game = &games[game_count];
    game->winner_slot = winner_slot;
    game->p1_char = p1_char;
    game->p2_char = p2_char;
    game->stage = stage;
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
            appendGame(1, CHAR_UNKNOWN, CHAR_UNKNOWN, 0);
        } else if (cdir == CDIR_RIGHT) {
            appendGame(2, CHAR_UNKNOWN, CHAR_UNKNOWN, 0);
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

/* "P3" for the port playing as `entrant` (1 or 2), "" while unknown. */
static const char* portLabel(int entrant)
{
    static const char* const labels[4] = { "P1", "P2", "P3", "P4" };
    int port = entrantPort(entrant);
    return port < 0 ? "" : labels[port];
}

/* Automatic scoring at game end (design.md sec 12, user 2026-09-22). The
 * vanilla GS_VS exit fills the scene's MatchEnd (outcome + per-slot standings:
 * type, nametag, stocks, percent), so lbTourney_MatchExit reads it after the
 * vanilla handler and decides the game there; the game is appended and sent
 * on the first CSS frame back (where the relay is polled), unless the game was
 * a handwarmer. The C-stick binds stay for corrections (undo / re-score). */
struct lbTourney_EndMelee { /* mirrors gmvs.c EndMeleeData */
    u32 x0, x4, x8;
    struct MatchEnd me;
};
#define LB_TOURNEY_AUTO_NOTE_FRAMES (5 * 60)

static void setAutoNote(const char* msg)
{
    int i;
    for (i = 0; i < (int) sizeof(auto_note) - 1 && msg[i] != '\0'; i++) {
        auto_note[i] = msg[i];
    }
    auto_note[i] = '\0';
    auto_note_frames = LB_TOURNEY_AUTO_NOTE_FRAMES;
}

static void autoScoreFromMatch(const struct MatchEnd* me)
{
    int slots[GM_MAX_PLAYERS];
    u8 tags[GM_MAX_PLAYERS];
    u8 who[2];
    int n = 0;
    int i;
    int w;

    auto_pending = 0;
    if (me->outcome == OUTCOME_NO_CONTEST) {
        setAutoNote("NO CONTEST - NOT SCORED");
        return;
    }
    if (me->outcome != OUTCOME_TIMEOUT && me->outcome != OUTCOME_ELIMINATION &&
        me->outcome != OUTCOME_TEAM_ELIMINATION)
    {
        return;
    }
    for (i = 0; i < GM_MAX_PLAYERS; i++) {
        if (me->player_standings[i].pkind == Gm_PKind_Human) {
            if (n < GM_MAX_PLAYERS) {
                slots[n] = i;
                tags[n] = me->player_standings[i].x4; /* nametag slot */
            }
            n++;
        }
    }
    if (n != 2) {
        setAutoNote("AUTO-SCORE NEEDS 2 PLAYERS");
        return;
    }
    assignEntrants(tags, 2, who);
    if (who[0] == 0 || who[1] == 0) {
        setAutoNote("PICK A TAG TO AUTO-SCORE");
        return;
    }
    /* Per-game character and stage (R13): the standings' ckind is the CSS
     * ckind (external id) and the stage is still in the start rules here. */
    auto_chars[who[0] - 1] = (u8) me->player_standings[slots[0]].ckind;
    auto_chars[who[1] - 1] = (u8) me->player_standings[slots[1]].ckind;
    auto_stage = (u8) gm_GetStartMeleeRules()->stkind;
    /* Stock mode: the survivor; on time-out more stocks, then less damage. */
    {
        const struct MatchPlayerData* a = &me->player_standings[slots[0]];
        const struct MatchPlayerData* b = &me->player_standings[slots[1]];
        if (a->stocks != b->stocks) {
            w = a->stocks > b->stocks ? 0 : 1;
        } else if (a->percent != b->percent) {
            w = a->percent < b->percent ? 0 : 1;
        } else {
            setAutoNote("TIE - SCORE IT MANUALLY");
            return;
        }
    }
    auto_pending = who[w];
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

    css_text = HSD_SisLib_803A6754(lbButton_Font(), css_ctx);
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
    } else if (auto_note_frames > 0) {
        status = auto_note; /* what auto-score did, or why it did not */
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
    vs_text = HSD_SisLib_803A6754(lbButton_Font(), vs_ctx);
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
                vs_ctx = HSD_SisLib_803A611C(lbButton_Font(), NULL, 9, 0xD,
                                             0, 0xE, 0, 0x13);
                lbButton_InstallFont();
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
    /* Vanilla first: it fills the exit data's MatchEnd (outcome, standings). */
    gm_Scene_Vs_OnExit(arg);
    if (has_set && !handwarmer && arg != NULL) {
        autoScoreFromMatch(&((struct lbTourney_EndMelee*) arg)->me);
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
    /* Venue mods (UCF, neutral spawns, striking, stealth nametag, the D-pad
     * rumble toggle, audio) are Nintendont's / Dolphin's gecko codes on the
     * vanilla DOL; the module adds nothing there. One consequence: the venue's
     * D-pad toggle writes the PORT pref, but Melee takes a tagged player's
     * in-match rumble from the TAG (gm_RumbleEnabledForPlayer) and the
     * kiosk's seeded tags start off - so the picked tags mirror their port's
     * pref every CSS frame. */
    {
        int port;
        if (has_set) {
            (void) tuneInputs();
        }
        for (port = 0; port < 4; port++) {
            int slot = mnCharSel_PortNametag(port);
            if (slot == 0 || slot == 1) {
                GetPersistentNameData(slot)->rumble_enabled =
                    GetRumbleSettingOfPort(port) ? true : false;
            }
        }
    }
    if (has_set) {
        if (css_ctx < 0) {
            css_ctx = HSD_SisLib_803A611C(lbButton_Font(), NULL, 9, 0xD, 0,
                                          0xE, 0, 0x13);
            lbButton_InstallFont();
            css_dirty = true;
        }
        if (match_seen) {
            /* Back from a game: a handwarmer flag covered exactly that game;
             * a decided game is scored now (sends REPORT_SCORE). */
            match_seen = false;
            handwarmer = false;
            css_dirty = true;
            if (auto_pending != 0) {
                char note[40];
                char tag[TAG_LEN + 1];
                int i;
                const char* src =
                    auto_pending == 1 ? cur_set.p1_tag : cur_set.p2_tag;
                appendGame(auto_pending, auto_chars[0], auto_chars[1],
                           auto_stage);
                memcpy(tag, src, TAG_LEN);
                tag[TAG_LEN] = '\0';
                /* "GAME 3 TO MANGO" */
                memcpy(note, "GAME ", 5);
                note[5] = (char) ('0' + game_count);
                memcpy(note + 6, " TO ", 4);
                for (i = 0; tag[i] != '\0' && i < TAG_LEN; i++) {
                    note[10 + i] = tag[i];
                }
                note[10 + i] = '\0';
                setAutoNote(note);
                auto_pending = 0;
            }
        }
        if (auto_note_frames > 0 && --auto_note_frames == 0) {
            css_dirty = true;
        }
        /* The port <-> nametag pairing changes as players pick tags. */
        {
            static int shown_p1_port = -2, shown_p2_port = -2;
            int a = portWithTag(0), b = portWithTag(1);
            if (a != shown_p1_port || b != shown_p2_port) {
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
