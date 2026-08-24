/*
 * `popcorn-dev` - the same game with the flags the harness needs.
 *
 * None of this is the port. `popcorn` takes what the original takes and
 * nothing else; everything that exists to *check* the port is here, so that
 * the deliverable's command line stays a fact about the deliverable rather
 * than a place where debugging options accumulate.
 *
 *   --autoplay [SEED]    play by itself, through the mouse - the same bot
 *                        autoplay.py drives the port with, but without the
 *                        Python process, the emulator beside it and the pipe
 *   --cmdline NAME       the level file, as `popcorn NAME` takes it
 *   --scale N            window scale
 *   --speed N            as if POPSPEED had been run with N
 *   --run-ms N           stop after N milliseconds of wall clock
 *   --shot FILE          write the screen there when the deadline is reached
 *   --dump-vram FILE     likewise, the raw 0xb8000 aperture
 *   --dump-image FILE    write the unpacked load image and exit, which is how
 *                        exepack.c is checked against unpack_popcorn.py
 *   --keys SCAN@MS,...   press scan codes at wall-clock times
 *   --verify IN OUT      run one routine on a captured state (verify.py)
 *   --lockstep STATE     the frame protocol sidebyside.py and autoplay.py
 *                        drive, resuming from a captured state
 *   --lockstep-sync-scroll / -endgame / -results
 *                        extra sync points, for the screens that are drawn
 *                        outside the play loop and are otherwise compared by
 *                        nothing at all
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"

int32_t main(int32_t argc, char **argv)
{
    const char *dump = NULL;
    int32_t scale = 3;
    uint32_t speed = 0;          /* as if POPSPEED had never been run */
    uint32_t run_ms = 0;
    const char *shot = NULL;
    const char *vram = NULL;
    const char *keys = NULL;
    const char *levels = NULL;          /* --cmdline, as in POPCORN POPTAB */
    const char *lockstep = NULL;
    int32_t extra_sync = 0;
    for (int32_t i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--lockstep") && i + 1 < argc)
            lockstep = argv[++i];
        else if (!strcmp(argv[i], "--lockstep-sync-scroll"))
            extra_sync |= SYNC_SCROLL;
        else if (!strcmp(argv[i], "--lockstep-sync-endgame"))
            extra_sync |= SYNC_ENDGAME;
        else if (!strcmp(argv[i], "--lockstep-sync-results"))
            extra_sync |= SYNC_RESULTS;
        else if (!strcmp(argv[i], "--lockstep-sync-curtain"))
            extra_sync |= SYNC_CURTAIN;
        else if (!strcmp(argv[i], "--verify") && i + 2 < argc)
            return verify_main(argv[i + 1], argv[i + 2]);
        else if (!strcmp(argv[i], "--dump-image") && i + 1 < argc)
            dump = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc)
            scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc)
            speed = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--run-ms") && i + 1 < argc)
            run_ms = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc)
            shot = argv[++i];
        else if (!strcmp(argv[i], "--dump-vram") && i + 1 < argc)
            vram = argv[++i];
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc)
            keys = argv[++i];
        else if (!strcmp(argv[i], "--autoplay")) {
            uint32_t seed = 0;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                seed = (uint32_t)strtoul(argv[++i], NULL, 10);
            autoplay_enable(seed);
        }
        else if (!strcmp(argv[i], "--cmdline") && i + 1 < argc)
            levels = argv[++i];
        else {
            fprintf(stderr,
                    "usage: %s [--autoplay [SEED]] [--cmdline NAME]\n"
                    "       %s [--scale N] [--speed N]\n"
                    "       %s [--run-ms N] [--shot FILE] [--dump-vram FILE]\n"
                    "       %s [--dump-image FILE] [--keys SCAN@MS,...]\n"
                    "       %s --verify STATE-IN RESULT-OUT\n"
                    "       %s --lockstep STATE [--lockstep-sync-...]\n"
                    "\n"
                    "To play, use popcorn.\n",
                    argv[0], argv[0], argv[0], argv[0], argv[0],
                    argv[0]);
            return 2;
        }
    }

    /* Lockstep brings its own image, in the state the emulator handed over,
     * and must put nothing but frames on stdout. The game still saves high
     * scores here, so the directory has to be settled before it runs. */
    if (lockstep) {
            io_lockstep_extra_sync(extra_sync);
        return lockstep_main(lockstep);
    }

    size_t len = popcorn_load_image();
    if (!len)
        return 1;

    if (dump) {
        FILE *f = fopen(dump, "wb");
        if (!f) {
            perror(dump);
            return 1;
        }
        fwrite(g_image, 1, len, f);
        fclose(f);
        printf("popcorn: wrote %s\n", dump);
        return 0;
    }

    if (!io_init(scale))
        return 1;
    io_set_deadline(run_ms, shot, vram);
    /* --keys 3b@30000,39@34000 : scan code, then when to press it. */
    for (const char *k = keys; k && *k; ) {
        uint32_t scan = (uint32_t)strtoul(k, (char **)&k, 16);
        if (*k == '@')
            k++;
        uint32_t ms = (uint32_t)strtoul(k, (char **)&k, 10);
        io_script_key(scan, ms);
        if (*k == ',')
            k++;
        else
            break;
    }
    if (getenv("POPCORN_TRACE_STUBS"))
        fprintf(stderr, "popcorn: high scores in %s%s\n",
                g_dir, (const char *)(g_image + 0x141c));
    game_main(g_dir, speed, levels);
    io_shutdown();
    free(g_image);
    return 0;
}
