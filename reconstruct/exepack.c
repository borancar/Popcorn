/*
 * Recover POPCORN.EXE's load image at run time.
 *
 * The port needs the game's data - its sprites, fonts, level tables and
 * strings all live in the first 0x1ac20 bytes of the image - and that data is
 * not distributed with this source.  So the port reads the player's own copy
 * of POPCORN.EXE and unpacks it, which is a hundred lines of RLE and costs a
 * millisecond at startup.
 *
 * The format is Microsoft EXEPACK.  A 16-byte header sits at CS:0 with the
 * real entry point, the unpacked length, and an 'RB' signature; the packed
 * data runs from image offset 0 up to the header, and is decoded *backwards*
 * into a buffer of the unpacked length.  Each command is read backwards as
 *
 *     [fill byte]  length_lo length_hi  command      (0xb0/0xb1: run of one byte)
 *     ...bytes...  length_lo length_hi  command      (0xb2/0xb3: literal copy)
 *
 * with bit 0 of the command marking the last one.  Trailing 0xff bytes pad the
 * packed data out to a paragraph and are skipped.
 *
 * Read alongside ../unpack_popcorn.py, which does the same job by running the
 * program's own unpacker stub under an emulator - that is the reference, and
 * ../validate.py proves the two agree.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"

#define EXEPACK_SIG 0x4252u             /* 'RB' */

static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }

unsigned char *exepack_load(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "popcorn: cannot open %s\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long file_len = ftell(f);
    rewind(f);
    if (file_len < 0x40) { fclose(f); return NULL; }

    unsigned char *file = malloc((size_t)file_len);
    if (!file || fread(file, 1, (size_t)file_len, f) != (size_t)file_len) {
        fclose(f);
        free(file);
        return NULL;
    }
    fclose(f);

    if (rd16(file) != 0x5a4d) {              /* 'MZ' */
        fprintf(stderr, "popcorn: %s is not an MZ executable\n", path);
        free(file);
        return NULL;
    }
    unsigned cblp = rd16(file + 2), cp = rd16(file + 4);
    unsigned hdr = rd16(file + 8) * 16u;
    unsigned ip = rd16(file + 20), cs = rd16(file + 22);
    size_t img_len = (cblp ? (size_t)(cp - 1) * 512 + cblp : (size_t)cp * 512);
    if (img_len > (size_t)file_len || hdr >= img_len) {
        fprintf(stderr, "popcorn: %s has a malformed MZ header\n", path);
        free(file);
        return NULL;
    }
    unsigned char *img = file + hdr;
    size_t packed_end = (size_t)cs * 16;      /* the stub starts here */

    /* The signature sits a fixed distance from the end of the header, and two
     * header lengths exist - the longer one inserts skip_len before it. Try
     * both and take whichever lands on 'RB'; Popcorn uses the short one. */
    size_t hsize = 0;
    for (size_t try_ = 18; try_ >= 16; try_ -= 2) {
        if (packed_end + try_ <= img_len - hdr &&
            rd16(img + packed_end + try_ - 2) == EXEPACK_SIG) {
            hsize = try_;
            break;
        }
    }
    if (!hsize) {
        fprintf(stderr, "popcorn: %s is not EXEPACK-compressed "
                        "(no 'RB' signature)\n", path);
        free(file);
        return NULL;
    }
    if (ip != hsize)
        fprintf(stderr, "popcorn: note: entry IP %#x is not the header size "
                        "%#zx\n", ip, hsize);

    size_t dest_len = (size_t)rd16(img + packed_end + 12) * 16;
    unsigned char *out = calloc(1, dest_len ? dest_len : 1);
    if (!out) { free(file); return NULL; }

    /* Decode backwards: source walks down from the last non-padding byte of
     * the packed data, destination down from the end of the output. */
    size_t src = packed_end;
    while (src > 0 && img[src - 1] == 0xff)
        src--;
    size_t dp = dest_len;
    int ok = 0;

    while (src >= 3) {
        unsigned cmd = img[--src];
        unsigned len = (unsigned)img[src - 1] << 8 | img[src - 2];
        src -= 2;
        if ((cmd & 0xfe) == 0xb0) {                  /* run of one byte */
            if (src < 1 || dp < len) break;
            unsigned char b = img[--src];
            dp -= len;
            memset(out + dp, b, len);
        } else if ((cmd & 0xfe) == 0xb2) {           /* literal copy */
            if (src < len || dp < len) break;
            src -= len;
            dp -= len;
            memcpy(out + dp, img + src, len);
        } else {
            break;                                   /* not a command */
        }
        if (cmd & 1) {                               /* last command */
            ok = 1;
            break;
        }
    }
    free(file);

    if (!ok || dp != 0) {
        fprintf(stderr, "popcorn: %s did not decompress cleanly "
                        "(%zu bytes of output unfilled)\n", path, dp);
        free(out);
        return NULL;
    }
    if (out_len)
        *out_len = dest_len;
    return out;
}
