/*
 * `popcorn-dev` - the same game with the flags the harness needs.
 *
 * None of this is the port. `popcorn` takes what the original takes and
 * nothing else; everything that exists to *check* the port is here, so that
 * the deliverable's command line stays a fact about the deliverable rather
 * than a place where debugging options accumulate.
 *
 *   --resume FILE        start from a snapshot.py capture, played by the
 *                        port itself rather than by a driver
 *   --level N            start a game on level N (0-49) instead of the
 *                        first, so what goes wrong deep in the game can be
 *                        watched without playing to it
 *   --autoplay [SEED]    play by itself, through the mouse - the same bot
 *                        autoplay.py drives the port with, but without the
 *                        Python process, the emulator beside it and the pipe
 *   --cmdline NAME       the level file, as `popcorn NAME` takes it
 *   --scale N            window scale
 *   --play-hz N          play-loop rate; 326 is the measured original
 *   --run-ms N           stop after N milliseconds of wall clock
 *   --shot FILE          write the screen there when the deadline is reached
 *   --dump-vram FILE     likewise, the raw 0xb8000 aperture
 *   --dump-image FILE    write the unpacked load image and exit, which is how
 *                        exepack.c is checked - or, with --run-ms, write it
 *                        when the run ends, to see what a session changed
 *                        exepack.c is checked against unpack_popcorn.py
 *   --keys SCAN@MS,...   press scan codes at wall-clock times; `^` before a
 *                        scan code presses it with shift
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
#include <setjmp.h>

#include "game.h"

/* Resume a snapshot.py capture directly, without a driver.
 *
 * The lockstep protocol can already start the port anywhere, but it drives
 * the mouse - so a state reached through it is played by whatever is at the
 * other end of the pipe, never by the port's own bot. The states worth
 * watching are exactly the ones that are hard to reach: the end-of-level
 * bonus needs a + capsule, 2 chances in 255 and never from a hatch.
 *
 *     popcorn-dev --autoplay --resume bonus.snap
 *
 * PSNP: the magic, the level and frame, fourteen registers, the tick count,
 * then the image and the screen with their lengths. CS and IP are among the
 * registers, which is what says where to rejoin - the same three places
 * lockstep knows, since they are the same three routines. */
static int32_t rd32le(FILE *f, uint32_t *out)
{
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4)
        return 0;
    *out = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
    return 1;
}

static int32_t resume_snapshot(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 1;
    }
    char magic[4];
    uint32_t level, frame, ticks, len;
    uint8_t regs[28];                   /* 14 words */
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "PSNP", 4) ||
        !rd32le(f, &level) || !rd32le(f, &frame) ||
        fread(regs, 1, sizeof regs, f) != sizeof regs ||
        !rd32le(f, &ticks) || !rd32le(f, &len)) {
        fprintf(stderr, "%s: not a snapshot\n", path);
        fclose(f);
        return 1;
    }
    g_image = malloc(len);
    if (!g_image || fread(g_image, 1, len, f) != len) {
        fprintf(stderr, "%s: truncated image\n", path);
        fclose(f);
        return 1;
    }
    uint32_t vlen;
    if (!rd32le(f, &vlen) || vlen != CGA_SIZE ||
        fread(g_vram, 1, CGA_SIZE, f) != CGA_SIZE) {
        fprintf(stderr, "%s: truncated screen\n", path);
        fclose(f);
        return 1;
    }
    fclose(f);
    io_set_ticks(ticks);

    /* Where to rejoin comes from the **game's own variables**, not from the
     * captured CS:IP. The registers are where the emulator happened to be
     * stopped, which is an accident of how the capture was taken; [0x13cc],
     * [0x13ca], [0x13c9] and the cells at 0x2f18 are what the situation *is*,
     * and they are in the image. Snapshots are taken at level boundaries, so
     * rejoining play_session's level loop puts the game where the capture
     * was - and does it the same way lockstep's own resume does, which is the
     * path that has four million compared frames behind it. */
    (void)regs;
    printf("popcorn: %s -> level %u, lives %u, score %.6s\n",
           path, level, g_image[0x13c9], (const char *)(g_image + 0x13cd));

    g_resume_at_frame_top = 1;
    g_resume_in_session = 1;
    if (setjmp(g_back_to_menu) == 0)
        play_session();
    return 0;
}

int32_t main(int32_t argc, char **argv)
{
    const char *dump = NULL;
    int32_t scale = 3;
    uint32_t run_ms = 0;
    const char *shot = NULL;
    const char *vram = NULL;
    const char *keys = NULL;
    const char *levels = NULL;          /* --cmdline, as in POPCORN POPTAB */
    const char *lockstep = NULL;
    const char *resume = NULL;
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
        else if (!strcmp(argv[i], "--lockstep-sync-ending"))
            extra_sync |= SYNC_ENDING;
        else if (!strcmp(argv[i], "--lockstep-sync-intro"))
            extra_sync |= SYNC_INTRO;
        else if (!strcmp(argv[i], "--verify") && i + 2 < argc)
            return verify_main(argv[i + 1], argv[i + 2]);
        else if (!strcmp(argv[i], "--dump-image") && i + 1 < argc)
            dump = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc)
            scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--play-hz") && i + 1 < argc)
            g_play_hz = (uint32_t)atoi(argv[++i]);
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
        else if (!strcmp(argv[i], "--resume") && i + 1 < argc)
            resume = argv[++i];
        else if (!strcmp(argv[i], "--level") && i + 1 < argc) {
            int32_t n = atoi(argv[++i]);
            g_start_level = n < 0 ? 0 : n > 0x31 ? 0x31 : n;
        }
        else if (!strcmp(argv[i], "--cmdline") && i + 1 < argc)
            levels = argv[++i];
        else {
            fprintf(stderr,
                    "usage: %s [--autoplay [SEED]] [--level N] "
                    "[--cmdline NAME]\n"
                    "       %s [--scale N] [--play-hz N]\n"
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

    if (resume) {
        /* The capture carries the image, so POPCORN.EXE is not needed. */
        if (!io_init(scale))
            return 1;
        io_set_deadline(run_ms, shot, vram);
        io_set_deadline_image(dump && run_ms ? dump : NULL);
        int32_t r = resume_snapshot(resume);
        io_shutdown();
        return r;
    }

    size_t len = popcorn_load_image();
    if (!len)
        return 1;

    /* With --run-ms, the image is wanted *after* the run - "did typing this
     * set that byte" is not a question the load image can answer. Without it,
     * dump and exit, which is how exepack.c is checked. */
    if (dump && !run_ms) {
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
    io_set_deadline_image(dump && run_ms ? dump : NULL);
    /* --keys 3b@30000,39@34000 : scan code, then when to press it. */
    for (const char *k = keys; k && *k; ) {
        /* `^` before the scan code presses it with shift, which is the only
         * way a script types a mixed-case string - the cheat at 0x3f0b is
         * `LACRAL software`, and cheat_match compares byte for byte. */
        int32_t shift = 0;
        if (*k == '^') { shift = 1; k++; }
        uint32_t scan = (uint32_t)strtoul(k, (char **)&k, 16);
        if (*k == '@')
            k++;
        uint32_t ms = (uint32_t)strtoul(k, (char **)&k, 10);
        io_script_key_shift(scan, ms, shift);
        if (*k == ',')
            k++;
        else
            break;
    }
    if (getenv("POPCORN_TRACE_STUBS"))
        fprintf(stderr, "popcorn: high scores in %s%s\n",
                g_dir, (const char *)(g_image + 0x141c));
    game_main(g_dir, levels);
    io_shutdown();
    free(g_image);
    return 0;
}
