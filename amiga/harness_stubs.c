/*
 * What the Amiga binary leaves out.
 *
 * The SDL build links the checking harness - lockstep, autoplay, verify -
 * into both binaries because the game's own code calls into it.  The Amiga
 * build ships the game alone; these are the no-op definitions game.c
 * reaches for.  Lockstep is never active here, the autoplay bot is off,
 * and the random-number log has nowhere to go.
 */
#include <stdint.h>

#include "game.h"

int32_t io_lockstep(void) { return 0; }

uint32_t io_lockstep_mouse_x(void) { return 320; }

uint32_t io_lockstep_buttons(void) { return 0; }

void io_lockstep_warp(uint32_t x) { (void)x; }

void io_lockstep_extra_sync(int32_t mask) { (void)mask; }

void io_frame_sync(void) {}

void io_frame_sync_extra(int32_t which) { (void)which; }

void io_log_random(uint32_t dl) { (void)dl; }

void autoplay_enable(uint32_t seed) { (void)seed; }

int32_t autoplay_on(void) { return 0; }

void autoplay_step(void) {}
