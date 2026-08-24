/*
 * What is not transcribed. There is one thing left in it.
 *
 * This file was the to-do list: a routine the port could not yet run got a
 * stub here so the game could go around it, and the screen it was responsible
 * for was simply absent. Every one of them has landed, so what remains is not
 * a gap - it is a safety net for an entity handler at an address that is in no
 * table, which should never fire.
 *
 * The two screens the port deliberately does not have - the boss key and
 * redefine-keys - are **not** here. They are no-ops in game.c with a comment
 * saying why, because they are decisions rather than work outstanding, and
 * putting them here would make port_coverage.py report them as missing.
 *
 * A stub is never silently correct - `POPCORN_TRACE_STUBS=1` in the
 * environment prints each one the first time it is reached, so "that screen is
 * blank" and "that routine is missing" are the same observation.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"

static int32_t trace = -1;

static void note(const char *name, uint32_t off)
{
    if (trace < 0)
        trace = getenv("POPCORN_TRACE_STUBS") != NULL;
    if (!trace)
        return;
    /* Once each: a stub inside a frame loop would otherwise flood. */
    static const char *seen[64];
    static int32_t n;
    for (int32_t i = 0; i < n; i++)
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
    void name(uint32_t bx) { note(#name, off); (void)bx; }

/* An entity whose handler has not been transcribed. Doing nothing leaves it in
 * the list for ever, so it is unlinked - the animation is lost but the list
 * does not fill up and stall the game. */
void entity_unknown(uint32_t bx)
{
    note("entity_unknown", 0x1b5e);
    (void)bx;
    g_image[ENTITY_REMOVE] = 1;
}



/* The player-name boxes return 0xff for "abort", anything else to go on. With
 * no implementation there is nobody to abort, so it reports a start. */

