/*
 * Routines the port has not transcribed yet.
 *
 * Each is here so the game can run around it, and each says what it is and
 * where to read it. They are **not** substitutes: a stub does nothing, and the
 * screen or the behaviour it was responsible for is simply absent. STATUS.md
 * keeps the running count, and this file shrinks as they land.
 *
 * A stub is never silently correct - `POPCORN_TRACE_STUBS=1` in the
 * environment prints each one the first time it is reached, so "that screen is
 * blank" and "that routine is missing" are the same observation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"

static int trace = -1;

static void note(const char *name, unsigned off)
{
    if (trace < 0)
        trace = getenv("POPCORN_TRACE_STUBS") != NULL;
    if (!trace)
        return;
    /* Once each: a stub inside a frame loop would otherwise flood. */
    static const char *seen[64];
    static int n;
    for (int i = 0; i < n; i++)
        if (seen[i] == name)
            return;
    if (n < 64)
        seen[n++] = name;
    fprintf(stderr, "popcorn: stub %s (1ac2:%04x)\n", name, off);
}

#define STUB(name, off, what) \
    void name(void) { note(#name, off); }

/* The same, for a handler that takes its node. */
#define STUB2(name, off) \
    void name(unsigned bx) { note(#name, off); (void)bx; }

/* --- the menu's live decoration ------------------------------------- */
STUB(menu_particles_init, 0x5476, "80 objects of 16 bytes at 0x148d")
STUB(menu_particles_tick, 0x53c2, "steps them; called once per menu frame")
STUB(menu_banner_tick,    0x50df, "the scrolling text on the character's belly")
STUB(menu_arrow,          0x490d, "moves the arrow between Souris and Clavier")

/* --- the screens behind the function keys ---------------------------- */
STUB(screen_define_keys,  0x1581, "F5: read three scan codes into 0x2d4f")
STUB(screen_high_scores,  0x4e1a, "F6: the hall of fame")
STUB(palette_cycle,       0x5196, "F8: rotate the CGA colour-select register")
STUB(menu_extra,          0x5171, "drawn each frame unless [0x3f1b] is 1")
STUB(employee_enter,      0x4ae0, "F10, the `touche speciale pour employes`")
STUB(employee_leave,      0x4b4f, "and its exit")

/* --- getting into a game -------------------------------------------- */
STUB(play_frame,          0x1212, "draws the playfield surround")
STUB(demo_start,          0x1509, "and starts it")
STUB(play_prepare,        0xcc5,  "after the names are entered")
STUB(level_load_file,     0x08c8, "reads a .PPC level set named on the command line")

/* --- called from the play loop, which is transcribed ------------------ */
STUB(play_teardown,       0x41d4, "tidies up when a level ends")
STUB(bonus_spawn,         0x3d95, "drops a bonus capsule")
STUB(demo_input_step,     0x1a6f, "advances the recorded-input cursor")


void cell_special(unsigned row, unsigned di)
{
    note("cell_special", 0x41e5);
    (void)row; (void)di;
}


/* --- called from play_session, which is transcribed ------------------- */
STUB(screen_game_over,      0x0473, "GAME OVER")
STUB(screen_end_of_game,    0x0d2e, "the hall of fame, and back to the menu")
STUB(screen_all_levels_done, 0x5940, "finishing all fifty")

void brick_11(unsigned slot, unsigned ball) { note("brick_11", 0x2d68); (void)slot; (void)ball; }


/* An entity whose handler has not been transcribed. Doing nothing leaves it in
 * the list for ever, so it is unlinked - the animation is lost but the list
 * does not fill up and stall the game. */
void entity_unknown(unsigned bx)
{
    note("entity_unknown", 0x1b5e);
    (void)bx;
    g_image[ENTITY_REMOVE] = 1;
}

STUB(bonus_end_level,     0x2da0, "kind 8: throws the stack away into 0x4210")

int bonus_script(unsigned bx, unsigned *px, unsigned *py)
{
    note("bonus_script", 0x3c35);
    (void)bx; (void)px; (void)py;
    return 1;
}

/* The player-name boxes return 0xff for "abort", anything else to go on. With
 * no implementation there is nobody to abort, so it reports a start. */

