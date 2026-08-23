/*
 * Entry point: find the player's POPCORN.EXE, unpack it, and run.
 *
 * The game's data - every sprite, font, level table and string - lives in the
 * first 0x1ac20 bytes of that image and is not distributed here, so the port
 * reads it from the original executable at startup.  --dump-image writes what
 * it recovered, which is how the C unpacker is checked against the Python one
 * (they must agree byte for byte; see ../validate.py).
 */
/* realpath(3), which -std=c99 alone does not declare - and _POSIX_C_SOURCE
 * is not enough for it on glibc, where it sits behind __USE_XOPEN_EXTENDED.
 * The high-score directory has to be resolved rather than relative; see
 * set_game_dir. */
#define _XOPEN_SOURCE 700

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "game.h"

/* Where the high-score file lives: **beside this binary**, not beside
 * POPCORN.EXE. The game directory holds the player's own copy of the game and
 * the port has no business writing into it; popcorn.hsc there has not been
 * touched since 1996 and can stay that way.
 *
 * Both the read and the write have to agree, so it is worked out once and
 * g_dir is what everything uses - including lockstep, which used to return
 * before this ran and dropped a popcorn.hsc wherever the driver started.
 *
 * /proc/self/exe rather than argv[0]: it is right whether the port was found
 * on PATH, run through a symlink, or started with a relative name from
 * somewhere else. argv[0] resolved is the fallback for anywhere without it. */
static char g_dir_buf[512];

static void set_game_dir(const char *argv0)
{
    char buf[512];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = 0;
        snprintf(g_dir_buf, sizeof g_dir_buf, "%s", buf);
    } else {
        char *abs = argv0 ? realpath(argv0, NULL) : NULL;
        snprintf(g_dir_buf, sizeof g_dir_buf, "%s",
                 abs ? abs : (argv0 ? argv0 : ""));
        free(abs);
    }
    char *slash = strrchr(g_dir_buf, '/');
    if (slash)
        slash[1] = 0;
    else
        g_dir_buf[0] = 0;
    g_dir = g_dir_buf;
}

static const char *find_exe(void)
{
    static const char *candidates[] = {
        "../popcorn/popcorn.exe", "popcorn/popcorn.exe", "popcorn.exe",
        "../popcorn/POPCORN.EXE", "POPCORN.EXE",
    };
    const char *env = getenv("POPCORN_EXE");
    if (env)
        return env;
    for (size_t i = 0; i < sizeof candidates / sizeof *candidates; i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (f) {
            fclose(f);
            return candidates[i];
        }
    }
    return NULL;
}

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
        else if (!strcmp(argv[i], "--cmdline") && i + 1 < argc)
            levels = argv[++i];
        else {
            fprintf(stderr, "usage: %s [--scale N] [--dump-image FILE]\n"
                            "       %s --verify STATE-IN RESULT-OUT\n",
                    argv[0], argv[0]);
            return 2;
        }
    }

    /* Lockstep brings its own image, in the state the emulator handed over,
     * and must put nothing but frames on stdout. */
    if (lockstep) {
        /* Even here: the game still saves high scores, and without this it
         * saved them into whatever directory the driver was started from. */
        set_game_dir(argv[0]);
        io_lockstep_extra_sync(extra_sync);
        return lockstep_main(lockstep);
    }

    const char *path = find_exe();
    if (!path) {
        fprintf(stderr,
                "popcorn: cannot find POPCORN.EXE.\n"
                "         Put your own copy in ../popcorn/, or set "
                "POPCORN_EXE to its path.\n"
                "         The game is not distributed with this source.\n");
        return 1;
    }

    size_t len = 0;
    g_image = exepack_load(path, &len);
    if (!g_image)
        return 1;
    printf("popcorn: %s -> %zu bytes of load image\n", path, len);
    if (len != IMAGE_LEN)
        fprintf(stderr, "popcorn: note: expected %d bytes, got %zu\n",
                IMAGE_LEN, len);

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
    /* The game directory, for the high-score file: alongside the executable
     * the data came from.
     *
     * **Static, and g_dir points at it.** load_high_scores was given this and
     * hsc_save was given g_dir, which nothing ever assigned - so the port read
     * the file next to POPCORN.EXE and wrote one into whatever directory it
     * happened to be started from. Run `./popcorn` from reconstruct/ and it
     * read ../popcorn/popcorn.hsc and wrote reconstruct/popcorn.hsc, so a
     * score set in one session was gone by the next and the table never grew
     * past what shipped in the image. */
    set_game_dir(argv[0]);
    if (getenv("POPCORN_TRACE_STUBS"))
        fprintf(stderr, "popcorn: high scores in %s%s\n",
                g_dir, (const char *)(g_image + 0x141c));
    game_main(g_dir, speed, levels);
    io_shutdown();
    free(g_image);
    return 0;
}
