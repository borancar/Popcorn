/*
 * Where each segment starts, for the unpacked build.
 *
 * game_unpacked.h declares SEG_ASSETS, SEG_ANIMATIONS and SEG_RUNTIME as
 * extern constants rather than the shipped image's 0xc460, 0x14a10 and
 * 0x1ac20: without `packed`, global_t grows by its alignment padding and
 * every segment after it starts later. Only once image_t is complete can
 * that be worked out, and this is where it is - the same three lines
 * `gen_data.py --skip-padding` appends for the no-padding build.
 */
#include <stddef.h>

#include "game.h"

const int32_t popcorn_seg_assets = (int32_t)offsetof(image_t, seg_assets);
const int32_t popcorn_seg_animations = (int32_t)offsetof(image_t, seg_animations);
const int32_t popcorn_seg_runtime = (int32_t)offsetof(image_t, seg_runtime);
