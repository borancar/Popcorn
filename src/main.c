/*
 * `popcorn` - the game, with the command line the original has and no other.
 *
 *     popcorn                 the fifty levels built into POPCORN.EXE
 *     popcorn POPTAB          POPTAB.PPC instead, as POPGEN wrote it
 *
 * That is the whole of it. popcorn.doc documents no options, and the two
 * batch files the game shipped with are `POPCORN %1` and `popcorn lltf`, so
 * one optional level file is everything the original accepts.
 *
 * The name is handed on as DOS would hand on the command tail, and the game's
 * own code at 1ac2:0157 does the rest: leading dots dropped, `.PPC` appended
 * unless the name already carries an extension. `popcorn .LTF`, `popcorn LTF`
 * and `popcorn LTF.PPC` therefore all open LTF.PPC, which is the original's
 * behaviour rather than a convenience added here.
 *
 * Everything else the port can do - the harness flags, the screenshotting,
 * the lockstep protocol, --verify - lives in `popcorn-dev`. Keeping them out
 * of here is the point: this binary is what the port *is*, and its argument
 * list is part of that. See devmain.c.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"

/* The original has no scale: the window is whatever mode 05h is. Three makes
 * 320x200 legible on a modern screen and is not a behaviour. */
#define SCALE 3

int32_t main(int32_t argc, char **argv)
{
    const char *levels = NULL;

    if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
        fprintf(stderr,
                "usage: %s [LEVELFILE]\n"
                "\n"
                "  LEVELFILE   a .PPC level set, as `POPCORN POPTAB` loads\n"
                "              POPTAB.PPC. Without one the fifty levels in\n"
                "              POPCORN.EXE are played.\n"
                "\n"
                "This is the game. For the harness flags - lockstep, "
                "screenshots,\n--verify and the rest - use popcorn-dev.\n",
                argv[0]);
        return 2;
    }
    if (argc == 2)
        levels = argv[1];

    if (!popcorn_load_image())
        return 1;
    if (!io_init(SCALE))
        return 1;

    game_main(g_dir, levels);

    io_shutdown();
    free(g_image);
    return 0;
}
