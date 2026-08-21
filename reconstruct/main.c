/*
 * Entry point: find the player's POPCORN.EXE, unpack it, and run.
 *
 * The game's data - every sprite, font, level table and string - lives in the
 * first 0x1ac20 bytes of that image and is not distributed here, so the port
 * reads it from the original executable at startup.  --dump-image writes what
 * it recovered, which is how the C unpacker is checked against the Python one
 * (they must agree byte for byte; see ../validate.py).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"

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

int main(int argc, char **argv)
{
    const char *dump = NULL;
    int scale = 3;
    unsigned speed = 0;          /* as if POPSPEED had never been run */
    unsigned run_ms = 0;
    const char *shot = NULL;
    const char *vram = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--verify") && i + 2 < argc)
            return verify_main(argv[i + 1], argv[i + 2]);
        else if (!strcmp(argv[i], "--dump-image") && i + 1 < argc)
            dump = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc)
            scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc)
            speed = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--run-ms") && i + 1 < argc)
            run_ms = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc)
            shot = argv[++i];
        else if (!strcmp(argv[i], "--dump-vram") && i + 1 < argc)
            vram = argv[++i];
        else {
            fprintf(stderr, "usage: %s [--scale N] [--dump-image FILE]\n"
                            "       %s --verify STATE-IN RESULT-OUT\n",
                    argv[0], argv[0]);
            return 2;
        }
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
    /* The game directory, for the high-score file: alongside the executable
     * the data came from. */
    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash)
        slash[1] = 0;
    else
        dir[0] = 0;
    game_main(dir, speed);
    io_shutdown();
    free(g_image);
    return 0;
}
