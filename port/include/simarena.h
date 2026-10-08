#ifndef _IN_SIMARENA_H
#define _IN_SIMARENA_H

#include <PR/ultratypes.h>

// fixed-address memory for everything the game allocates; see port/src/simarena.c
#define SIM_ARENA_SIZE (112u * 1024u * 1024u)

void *simAlloc(u32 size);
void *simZeroAlloc(u32 size);
void *simRealloc(void *ptr, u32 size);
void simFree(void *ptr);
s32 simOwns(const void *ptr);

u8 *simArenaBase(void);
u32 simArenaHighWater(void);
void simArenaSetHighWater(u32 hw);

#endif
