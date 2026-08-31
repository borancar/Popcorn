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
 *
 * With one exception, `--rgbi`, because it is about what the game looks like
 * rather than about checking it. The game asks BIOS for mode 05h, and a real
 * CGA answers that by killing the colour burst and showing cyan, red and
 * white on an RGBI monitor whatever the palette register says. That is not
 * what its authors saw: an EGA or VGA board emulates the mode without the
 * quirk, and the game comes up in light cyan, light magenta and white, which
 * is what this draws by default. `--rgbi` asks for the CGA's own picture.
 * Nobody should have to set an environment variable to see the game the other
 * way round.
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
    int32_t rgbi = 0, bad = 0;

    for (int32_t i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rgbi"))
            rgbi = 1;
        else if (argv[i][0] == '-' || levels != NULL)
            bad = 1;
        else
            levels = argv[i];
    }
    if (bad) {
        fprintf(stderr,
                "usage: %s [--rgbi] [LEVELFILE]\n"
                "\n"
                "  LEVELFILE   a .PPC level set, as `POPCORN POPTAB` loads\n"
                "              POPTAB.PPC. Without one the fifty levels in\n"
                "              POPCORN.EXE are played.\n"
                "  --rgbi      the colours a real CGA gives mode 05h on an\n"
                "              RGBI monitor - cyan, red and white. The\n"
                "              default is what an EGA or VGA shows, which is\n"
                "              what the game's authors saw.\n"
                "\n"
                "This is the game. For the harness flags - lockstep, "
                "screenshots,\n--verify and the rest - use popcorn-dev.\n",
                argv[0]);
        return 2;
    }

    if (!popcorn_load_image())
        return 1;
    if (!io_init(SCALE))
        return 1;
    if (rgbi)
        io_set_rgbi(1);

    game_main(g_dir, levels);

    io_shutdown();
    free(g_image);
    return 0;
}
