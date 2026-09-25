#include "mntourney.h"

#include <Runtime/platform.h>

#include <string.h>

#include "forward.h"
#include "inlines.h"
#include "types.h"
#include <dolphin/os.h>
#include <dolphin/pad.h>
#include <melee/gm/forward.h>
#include <melee/gm/gm_1601.h>
#include <melee/gm/gm_1A36.h>
#include <melee/gm/gmmain_lib.h>
#include <melee/lb/lbbuttonglyph.h>
#include <melee/lb/lbrelayexi.h>
#include <melee/lb/lbtourney.h>
#include <melee/mn/mnmain.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjplink.h>
#include <sysdolphin/baselib/gobjproc.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/sislib.h>

u16 mnTourney_DescIndices[1] = { 0 };

/* Which menu-kind row hosts the Tournament menu. The DOL build appends its
 * own row (MENU_KIND_TOURNAMENT); the module, running on a vanilla DOL whose
 * table cannot grow, takes over MENU_KIND_TOY ("Trophies": its code region is
 * where the module lives, so that menu must never run anyway) - the loader
 * patches that row's description_indices / selection_count / think
 * (tools/module_hooks.txt). */
#ifdef TOURNAMENT_MODULE
#define TM_MENU_KIND MENU_KIND_TOY
#else
#define TM_MENU_KIND MENU_KIND_TOURNAMENT
#endif

/* Menu flow (design 6.1):
 *
 *   [Loading...]  LIST_SETS in flight
 *   Set list      up/down scroll, L/R first-letter tag filter, A confirm,
 *                 B back to main menu
 *   Confirm       "WR2  MANGO VS ZAIN - BO3  START?"; A sends START_SET
 *   Error         A retries the failed request, B goes back
 *
 * The relay device answers within the kernel's 3 s budget; the menu polls
 * once per frame and gives up after 5 s (an absent device reads junk poll
 * states forever, which lands here too). */

enum mnTourney_State {
    TM_OFF, /* menu not active */
    TM_LOADING,
    TM_LIST,
    TM_CONFIRM,
    TM_STARTING,
    TM_ERROR
};

#define TM_TIMEOUT_FRAMES (5 * 60)
#define TM_ROWS_VISIBLE 9
#define TM_MARGIN_X 64.0f /* left margin, inside the menu border */
/* Largest row count that fits the 4 KB poll buffer alongside the headers. */
#define TM_MAX_SETS                                                          \
    ((int) ((sizeof(((struct lbRelayExi_PollBuf*) 0)->payload) -             \
             sizeof(struct list_sets_resp)) /                                \
            sizeof(struct set_entry)))

static u8 tm_state = TM_OFF;
static struct set_entry tm_sets[MAX_SETS];
static u16 tm_count;
static u16 tm_sel;    /* cursor, an index into the filtered view */
static u16 tm_top;    /* first visible filtered row */
static u16 tm_chosen; /* tm_sets index picked on the confirm screen */
static char tm_filter; /* 0 = all sets, else 'A'..'Z' */
static u32 tm_timeout;
static u8 tm_retry_cmd; /* relay_cmd the error screen's A retries */
static char tm_errmsg[MSG_LEN + 1];
static bool tm_dirty;
/* When the main-menu think should drop straight into the set list: armed at
 * boot (static init) and whenever the CSS routes back here (END_SET, CSS-B).
 * A manual B-back from the list leaves it clear, so the main menu stays up. */
static bool tm_auto_enter = true;

/* Frames the main menu must run (rendering, so its textures become resident)
 * before an armed auto-enter fires. cooldown==0 alone proved too early on a
 * cold boot -- it still tore the menu down mid-texture-load and crashed in
 * __GXSetSUTexRegs (v9). A manual Z-enter, which happens seconds later, has
 * always been safe; this warm-up reproduces that safe timing. Reset whenever
 * auto-enter is re-armed (a CSS return re-initialises the menu scene). */
#define TM_BOOT_WARMUP_FRAMES 45
static int tm_boot_frames = 0;

/* Venue audio defaults (mono + music-off) are asserted once, the first time the
 * set list is up: a stable, fully-rendered menu frame (so no GX-transition
 * crash), AFTER the memcard save-load (so it isn't overwritten), and before any
 * match. sound_balance = 100 puts the SOUNDS<->MUSIC slider at all-sounds (music
 * off); OSSetSoundMode(0) forces mono. */
static bool tm_audio_set = false;

/* Kiosk: hide/show the main menu's visuals. Its background (class 4, plink 5,
 * MenMainBack_Top) and panel (class 5, plink 6, MenMainPanel_Top; the cursor
 * joints are its children) are fire-and-forget GObjs made by the matched
 * mnMain_Scene_OnEnter, whose returns are discarded - so they are found by
 * walking their plinks (plinklow_gobjs is the low-priority end; prev walks the
 * whole list) and matched by classifier. Only the root JObj's render flag
 * changes: the scene, camera and every proc keep running, so the boot warm-up
 * still does its job. Hidden for the warm-up only (no main-menu flash); shown
 * again the moment the set list comes up (its border frames the list) and on
 * B-back. */
static void setPlinkClassHidden(u8 link, u16 classifier, bool hide)
{
    HSD_GObj* g;
    for (g = plinklow_gobjs[link]; g != NULL; g = g->prev) {
        if (g->classifier != classifier || g->hsd_obj == NULL) {
            continue;
        }
        if (hide) {
            HSD_JObjSetFlagsAll((HSD_JObj*) g->hsd_obj, JOBJ_HIDDEN);
        } else {
            HSD_JObjClearFlagsAll((HSD_JObj*) g->hsd_obj, JOBJ_HIDDEN);
        }
    }
}

static void setMenuVisualsHidden(bool hide)
{
    /* Panel + cursor only. The backdrop (class 4, plink 5, MenMainBack_Top)
     * stays visible from frame 0: it is not the "main menu flash" (that is
     * the options panel), and the set list looks barren without it (user,
     * 2026-09-22). */
    setPlinkClassHidden(6, 5, hide);
}

/* SIS overlay, screen-space like the title screen's build timestamp.
 * Recreated per GS_MENU visit; the scene teardown frees the objects and
 * mnTourney_MenuSceneExit forgets them. */
static s32 tm_ctx = -1;
static HSD_Text* tm_text = NULL;

static void copyStr(char* dst, const char* src, int n)
{
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static char upperFirst(const char* tag)
{
    char c = tag[0];
    if (c >= 'a' && c <= 'z') {
        c -= 'a' - 'A';
    }
    return c;
}

static bool setMatchesFilter(const struct set_entry* set)
{
    if (tm_filter == 0) {
        return true;
    }
    return upperFirst(set->p1_tag) == tm_filter ||
           upperFirst(set->p2_tag) == tm_filter;
}

/* Fills out with tm_sets indices matching the filter; returns the count. */
static int filteredSets(u8* out)
{
    int i;
    int n = 0;
    for (i = 0; i < tm_count; i++) {
        if (setMatchesFilter(&tm_sets[i])) {
            out[n++] = i;
        }
    }
    return n;
}

/* Steps tm_filter through ALL plus each letter some tag starts with. */
static void stepFilter(int dir)
{
    char letters[27];
    bool seen[26];
    int i, n, cur;

    memset(seen, 0, sizeof(seen));
    for (i = 0; i < tm_count; i++) {
        char a = upperFirst(tm_sets[i].p1_tag);
        char b = upperFirst(tm_sets[i].p2_tag);
        if (a >= 'A' && a <= 'Z') {
            seen[a - 'A'] = true;
        }
        if (b >= 'A' && b <= 'Z') {
            seen[b - 'A'] = true;
        }
    }
    letters[0] = 0; /* ALL */
    n = 1;
    for (i = 0; i < 26; i++) {
        if (seen[i]) {
            letters[n++] = 'A' + i;
        }
    }
    cur = 0;
    for (i = 0; i < n; i++) {
        if (letters[i] == tm_filter) {
            cur = i;
        }
    }
    cur = (cur + dir + n) % n;
    tm_filter = letters[cur];
    tm_sel = 0;
    tm_top = 0;
}

static void destroyText(void)
{
    if (tm_text != NULL) {
        HSD_SisLib_803A5CC4(tm_text);
        tm_text = NULL;
    }
}

static int line(f32 x, f32 y, f32 scale, const char* str)
{
    int entry = HSD_SisLib_803A6B98(tm_text, x, y, "%s", str);
    HSD_SisLib_803A7548(tm_text, entry, scale, scale);
    return entry;
}

/* A line centred on the 640-wide screen, measured exactly from the font's
 * kerning table; fmt may carry #A/#B/... button icons (lbbuttonglyph.h). */
static void centred(f32 y, f32 scale, const char* fmt)
{
    f32 w = lbButton_Measure(scale, fmt);
    lbButton_Line(tm_text, 0.5f * (640.0f - w), y, scale, fmt);
}

static void drawSetLine(f32 y, bool cursor, const struct set_entry* set)
{
    char round[ROUND_LEN + 1];
    char p1[TAG_LEN + 1];
    char p2[TAG_LEN + 1];
    int entry;

    copyStr(round, set->round, ROUND_LEN);
    copyStr(p1, set->p1_tag, TAG_LEN);
    copyStr(p2, set->p2_tag, TAG_LEN);
    /* Player names first (the thing players scan for); round/phase trails. */
    entry = HSD_SisLib_803A6B98(tm_text, TM_MARGIN_X, y, "%s%s VS %s  BO%d  %s",
                                cursor ? "> " : "  ", p1, p2, set->best_of,
                                round);
    HSD_SisLib_803A7548(tm_text, entry, 0.55f, 0.55f);
}

static void redraw(void)
{
    u8 view[MAX_SETS];
    int n, i;
    f32 y;
    char buf[64];

    destroyText();
    tm_text = HSD_SisLib_803A6754(lbButton_Font(), tm_ctx);
    tm_text->default_kerning = 1;

    centred(50.0f, 0.72f, "TOURNAMENT");

    switch (tm_state) {
    case TM_LOADING:
        line(TM_MARGIN_X, 150.0f, 0.6f, "LOADING...");
        break;
    case TM_STARTING:
        line(TM_MARGIN_X, 150.0f, 0.6f, "STARTING SET...");
        break;
    case TM_LIST:
        n = filteredSets(view);
        if (tm_filter == 0) {
            lbButton_Line(tm_text, TM_MARGIN_X, 98.0f, 0.5f,
                          "FILTER: ALL   #L #R");
        } else {
            buf[0] = tm_filter;
            buf[1] = '\0';
            HSD_SisLib_803A7548(
                tm_text,
                HSD_SisLib_803A6B98(tm_text, TM_MARGIN_X, 98.0f, "FILTER: %s",
                                    buf),
                0.5f, 0.5f);
            lbButton_Line(tm_text,
                          TM_MARGIN_X + lbButton_Measure(0.5f, "FILTER: ALL"),
                          98.0f, 0.5f, "   #L #R");
        }
        if (n == 0) {
            line(TM_MARGIN_X, 150.0f, 0.6f, "NO SETS - PRESS Y TO REFRESH");
        }
        y = 134.0f;
        for (i = tm_top; i < n && i < tm_top + TM_ROWS_VISIBLE; i++) {
            drawSetLine(y, i == tm_sel, &tm_sets[view[i]]);
            y += 27.0f;
        }
        /* Hint bar, centred exactly, with button icons. */
        centred(394.0f, 0.36f,
                "#A START   #Z FRIENDLIES   #Y REFRESH   #B MENU");
        break;
    case TM_CONFIRM: {
        const struct set_entry* set = &tm_sets[tm_chosen];
        line(TM_MARGIN_X, 120.0f, 0.62f, "START THIS SET?");
        drawSetLine(180.0f, false, set);
        centred(394.0f, 0.36f, "#A YES    #B BACK");
        break;
    }
    case TM_ERROR:
        line(TM_MARGIN_X, 130.0f, 0.62f, "ERROR");
        line(TM_MARGIN_X, 174.0f, 0.52f, tm_errmsg);
        centred(394.0f, 0.36f, "#A RETRY    #B BACK");
        break;
    }
}

static void fail(const char* msg)
{
    copyStr(tm_errmsg, msg, MSG_LEN);
    tm_state = TM_ERROR;
    tm_dirty = true;
}

static void failFromResp(const struct relay_resp* resp)
{
    memcpy(tm_errmsg, resp->msg, MSG_LEN);
    tm_errmsg[MSG_LEN] = '\0';
    if (tm_errmsg[0] == '\0') {
        copyStr(tm_errmsg, "RELAY ERROR", MSG_LEN);
    }
    tm_state = TM_ERROR;
    tm_dirty = true;
}

static void sendList(void)
{
    tm_retry_cmd = CMD_LIST_SETS;
    tm_timeout = 0;
    if (lbRelayExi_Request(CMD_LIST_SETS, NULL, 0)) {
        tm_state = TM_LOADING;
    } else {
        fail("EXI ERROR");
    }
    tm_dirty = true;
}

static void sendStart(void)
{
    struct start_set_req req;
    req.set_id = tm_sets[tm_chosen].set_id;
    /* The kernel overwrites stream (and hdr.station) from tournament.cfg. */
    req.stream = 0;
    req._pad[0] = req._pad[1] = req._pad[2] = 0;

    tm_retry_cmd = CMD_START_SET;
    tm_timeout = 0;
    if (lbRelayExi_Request(CMD_START_SET, &req, sizeof(req))) {
        tm_state = TM_STARTING;
    } else {
        fail("EXI ERROR");
    }
    tm_dirty = true;
}

static void exitToMainMenu(void)
{
    destroyText();
    tm_state = TM_OFF;
    /* B-back: the player wants the real main menu, so show its visuals again
     * before its think takes over. */
    setMenuVisualsHidden(false);
    /* Frees this think GObj and spawns the main-menu think. */
    mn_80229894(MENU_KIND_MAIN, 0, 3);
}

static void acceptList(const struct lbRelayExi_PollBuf* r)
{
    const struct list_sets_resp* list =
        (const struct list_sets_resp*) r->payload;
    int count = list->count;
    if (count > MAX_SETS) {
        count = MAX_SETS;
    }
    if (count > TM_MAX_SETS) {
        count = TM_MAX_SETS;
    }
    tm_count = count;
    memcpy(tm_sets, list->sets, count * sizeof(struct set_entry));
    tm_sel = 0;
    tm_top = 0;
    tm_filter = 0;
    tm_state = TM_LIST;
    tm_dirty = true;
}

static void pollRelay(void)
{
    const struct lbRelayExi_PollBuf* r;
    s32 state = lbRelayExi_Poll();

    if (state < 0) {
        fail("EXI ERROR");
        return;
    }
    if (state == RELAY_ERROR) {
        fail("RELAY LINK ERROR");
        return;
    }
    if (state != RELAY_DONE) {
        if (++tm_timeout > TM_TIMEOUT_FRAMES) {
            lbRelayExi_Abort();
            fail("TIMEOUT - RELAY NOT ANSWERING");
        }
        return;
    }
    r = lbRelayExi_Response();
    if (r->hdr.magic[0] != RELAY_MAGIC_0 || r->hdr.magic[1] != RELAY_MAGIC_1 ||
        r->hdr.cmd != tm_retry_cmd)
    {
        fail("BAD RESPONSE");
        return;
    }
    if (r->resp.status != ST_OK) {
        failFromResp(&r->resp);
        return;
    }
    if (tm_retry_cmd == CMD_LIST_SETS) {
        acceptList(r);
    } else {
        /* START_SET accepted: hand the set to lbtourney and enter the CSS.
         * The scene teardown frees this think and the overlay. */
        lbTourney_SetCurrent(&tm_sets[tm_chosen]);
        tm_state = TM_OFF;
        mn_80229860(GM_VS);
    }
}

void mnTourney_Think(HSD_GObj* gobj)
{
    u8 view[MAX_SETS];
    int n;
    u64 buttons;
    UNUSED u8 _pad[8];

    (void) gobj;
    buttons = Menu_GetAllInputs();

    if (tm_ctx < 0) {
        tm_ctx = HSD_SisLib_803A611C(lbButton_Font(), NULL, 9, 0xD, 0, 0xE, 0,
                                     0x13);
        lbButton_InstallFont();
        tm_dirty = true;
    }

    /* Venue audio defaults, once the set list is stably up (see tm_audio_set).
     * Not in forceKioskDefaults: that runs inside the menu-enter transition,
     * where touching the mix crashed the GX texture path (bisected v12-v15). */
    if (!tm_audio_set && tm_state == TM_LIST) {
        tm_audio_set = true;
        OSSetSoundMode(0);                             /* mono */
        gmMainLib_GetGamePrefs()->sound_balance = 100; /* music off */
    }

    switch (tm_state) {
    case TM_LOADING:
        pollRelay();
        if (tm_state == TM_LOADING && (buttons & MenuInput_Back)) {
            sfxBack();
            lbRelayExi_Abort();
            exitToMainMenu();
            return;
        }
        break;
    case TM_STARTING:
        /* The request is committed; B is ignored until it resolves. */
        pollRelay();
        break;
    case TM_LIST:
        n = filteredSets(view);
        if (gm_GetButtonsTriggered(4) & PAD_TRIGGER_Z) {
            /* Friendlies: enter the CSS with no set active, so nothing is
             * reported. B on the CSS still returns here (kiosk routing). */
            sfxForward();
            lbTourney_ClearCurrent();
            tm_state = TM_OFF;
            mn_80229860(GM_VS);
            return;
        }
        if (gm_GetButtonsTriggered(4) & PAD_BUTTON_Y) {
            /* Refresh the set list (B is now "back to main menu"). */
            sfxForward();
            sendList();
            return;
        }
        if (buttons & MenuInput_Back) {
            sfxBack();
            exitToMainMenu();
            return;
        }
        if ((buttons & MenuInput_Confirm) && n > 0) {
            sfxForward();
            tm_chosen = view[tm_sel];
            tm_state = TM_CONFIRM;
            tm_dirty = true;
        } else if ((buttons & MenuInput_Up) && tm_sel > 0) {
            sfxMove();
            tm_sel--;
            if (tm_sel < tm_top) {
                tm_top = tm_sel;
            }
            tm_dirty = true;
        } else if ((buttons & MenuInput_Down) && n > 0 && tm_sel < n - 1) {
            sfxMove();
            tm_sel++;
            if (tm_sel >= tm_top + TM_ROWS_VISIBLE) {
                tm_top = tm_sel - (TM_ROWS_VISIBLE - 1);
            }
            tm_dirty = true;
        } else if (buttons & MenuInput_LTrigger) {
            sfxMove();
            stepFilter(-1);
            tm_dirty = true;
        } else if (buttons & MenuInput_RTrigger) {
            sfxMove();
            stepFilter(1);
            tm_dirty = true;
        }
        break;
    case TM_CONFIRM:
        if (buttons & MenuInput_Back) {
            sfxBack();
            tm_state = TM_LIST;
            tm_dirty = true;
        } else if (buttons & MenuInput_Confirm) {
            sfxForward();
            sendStart();
        }
        break;
    case TM_ERROR:
        if (buttons & MenuInput_Back) {
            sfxBack();
            if (tm_retry_cmd == CMD_START_SET) {
                tm_state = TM_LIST;
                tm_dirty = true;
            } else {
                exitToMainMenu();
                return;
            }
        } else if (buttons & MenuInput_Confirm) {
            sfxForward();
            if (tm_retry_cmd == CMD_LIST_SETS) {
                sendList();
            } else {
                sendStart();
            }
        }
        break;
    default:
        break;
    }

    if (tm_dirty) {
        tm_dirty = false;
        redraw();
    }
}

/* Force the venue's tournament state live each time we pass the main menu, so
 * it holds regardless of what the memory-card save has (design.md R12): all
 * characters unlocked, Stock mode, 4 stocks, 8:00, no items. Stages already
 * default to all-unlocked but we set the mask too for good measure. */
static void forceKioskDefaults(void)
{
    GameRules* rules = gmMainLib_GetGameRules();
    struct GamePrefs* prefs = gmMainLib_GetGamePrefs();

    rules->mode = 1;             /* Stock */
    rules->stock_count = 4;
    rules->stock_time_limit = 8; /* 8:00 in Stock mode (reads stock_time_limit) */
    rules->stage_sel = 0;        /* Choose: the SSS is shown (lbtourney flips
                                  * this to Random for a Z+X handwarmer start) */

    prefs->item_freq = 0xFF;     /* -1 (read as s8) = items OFF; 0 is lowest ON */
    prefs->item_mask = 0;
    /* Random-stage set = the singles legal six (Battlefield, Final Destination,
     * Fountain of Dreams, Yoshi's Story, Dream Land, Pokemon Stadium). This is
     * Magus's "Singles Stages" value for stage_mask (04 write of 0xE70000B0 to
     * DefaultGamePrefs+0x18). Manual stage picks are unaffected. */
    prefs->stage_mask = 0xE70000B0;

    /* NOTE: audio venue defaults are NOT forced here. Writing sound_balance to
     * LIVE prefs at this menu-enter point flips the music mix mid
     * scene-transition and crashes in the GX texture path (bisected v12-v15).
     * Both are asserted instead in mnTourney_Think once the set list is stably
     * up (tm_audio_set): a stable frame, after the memcard save-load (which
     * overwrites the boot default), before any match. The default template also
     * carries sound_balance = 100 as a no-memcard fallback. */

    gm_8016468C();               /* unlock all stages (the real unlock mask) */
    *gmMainLib_GetUnlockedCharactersBitmaskPtr() = 0xFFFF; /* all characters */
}

/* Leave the main menu for the Tournament submenu (sound-test style): assert
 * the kiosk rules/unlocks, swap cur_menu, spawn our think, free this think. */
static void enterTournament(HSD_GObj* gobj)
{
    HSD_GObjProc* proc;

    forceKioskDefaults();
    /* Entering the set list: show the menu panel again. It was hidden only
     * for the boot warm-up (no main-menu flash); on the tournament screen its
     * border frames the list (user, 2026-09-22: "bring back the border"). */
    setMenuVisualsHidden(false);
    mn_804D6BC8.cooldown = 5;
    mn_804A04F0.prev_menu = mn_804A04F0.cur_menu;
    mn_804A04F0.cur_menu = TM_MENU_KIND;
    mn_804A04F0.hovered_selection = 0;
    proc = HSD_GObj_SetupProc(GObj_Create(0, 1, 0x80), mnTourney_Think, 0);
    proc->flags_3 = HSD_GObj_804D783C;
    HSD_GObjFree(gobj);
    sendList();
}

void mnTourney_ArmAutoEnter(void)
{
    tm_auto_enter = true;
    tm_boot_frames = 0;
}

void mnTourney_MainMenuThink(HSD_GObj* gobj)
{
    /* Boot and every return from the CSS drop straight into the set list --
     * but only once the menu is ready (cooldown hits 0). Entering on the very
     * first frame renders half-initialised menu graphics and crashes in the
     * GX texture path; waiting for cooldown==0 is the safe point (a brief
     * main-menu flash; a zero-frame version needs the panel GObj hidden). */
    if (tm_auto_enter) {
        /* Let the menu render for a while first: entering the instant cooldown
         * hits 0 catches half-loaded menu textures and crashes in the GX
         * texture path (__GXSetSUTexRegs) on a cold boot. */
        if (tm_boot_frames < TM_BOOT_WARMUP_FRAMES) {
            if (tm_boot_frames == 0) {
                /* First warm-up frame: the menu visuals exist (made by the
                 * scene's OnEnter) but must never be seen. */
                setMenuVisualsHidden(true);
            }
            tm_boot_frames++;
            mn_8022DB10(gobj);
            return;
        }
        if (mn_804D6BC8.cooldown == 0) {
            tm_auto_enter = false;
            enterTournament(gobj);
            return;
        }
    }
    /* Otherwise the main menu is shown (the player backed out with B); Z
     * re-enters the Tournament screen. (A visible main-menu row needs an
     * MnMaAll asset edit -- deferred; Z is the interim entry.) */
    if (mn_804D6BC8.cooldown == 0 && (gm_GetButtonsTriggered(4) & PAD_TRIGGER_Z))
    {
        sfxForward();
        enterTournament(gobj);
        return;
    }
    mn_8022DB10(gobj);
}

void mnTourney_MenuSceneExit(void* exit_data)
{
    (void) exit_data;
    /* The scene teardown frees the canvas and text GObjs; just forget them. */
    tm_ctx = -1;
    tm_text = NULL;
    tm_state = TM_OFF;
}
