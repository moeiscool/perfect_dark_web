#include <stdlib.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "system.h"
#include "simarena.h"

/**
 * The simulation arena: one fixed block of memory that holds everything the game allocates
 * (the memp heap, room geometry, the ROM image and data loaded from it).
 *
 * Game state is full of absolute pointers, so for a netplay snapshot from one machine to be
 * usable on another, all of the game's memory has to be at the same addresses on both. The C
 * heap can't guarantee that (the browser also allocates for SDL/WebGL there, the server doesn't),
 * but a static block allocated with a deterministic allocator can: the same sequence of
 * allocations always produces the same addresses.
 *
 * Allocator: address-ordered first fit with block headers and coalescing on free, all inside the
 * arena (so the allocator's own state is part of a snapshot too).
 */

#define ALIGN 16
#define HDR_SIZE 16
#define FLAG_USED 1u

struct block {
	u32 size;    // total size including this header, multiple of ALIGN
	u32 flags;
	u32 prevsize; // size of the previous block in memory, 0 for the first
	u32 pad;
};

static u8 arena[SIM_ARENA_SIZE] __attribute__((aligned(64)));
static u32 arenaInit = 0;
static u32 arenaHighWater = 0; // offset of the end of the highest block ever used

static inline struct block *blockAt(u32 ofs)
{
	return (struct block *)(arena + ofs);
}

static inline u32 blockOfs(const struct block *b)
{
	return (u32)((const u8 *)b - arena);
}

static void arenaSetup(void)
{
	struct block *b = blockAt(0);
	b->size = SIM_ARENA_SIZE;
	b->flags = 0;
	b->prevsize = 0;
	arenaInit = 1;
}

static inline struct block *blockNext(struct block *b)
{
	const u32 ofs = blockOfs(b) + b->size;
	return ofs < SIM_ARENA_SIZE ? blockAt(ofs) : NULL;
}

static inline struct block *blockPrev(struct block *b)
{
	return b->prevsize ? blockAt(blockOfs(b) - b->prevsize) : NULL;
}

void *simAlloc(u32 size)
{
	if (!arenaInit) {
		arenaSetup();
	}

	const u32 need = ((size + HDR_SIZE + ALIGN - 1) / ALIGN) * ALIGN;

	for (u32 ofs = 0; ofs < SIM_ARENA_SIZE; ) {
		struct block *b = blockAt(ofs);

		if (!(b->flags & FLAG_USED) && b->size >= need) {
			// split if the rest is big enough to be useful
			if (b->size - need >= HDR_SIZE + ALIGN * 4) {
				struct block *rest = blockAt(ofs + need);
				struct block *after;
				rest->size = b->size - need;
				rest->flags = 0;
				rest->prevsize = need;
				b->size = need;
				after = blockNext(rest);
				if (after) {
					after->prevsize = rest->size;
				}
			}

			b->flags = FLAG_USED;

			if (ofs + b->size > arenaHighWater) {
				arenaHighWater = ofs + b->size;
			}

			return (u8 *)b + HDR_SIZE;
		}

		ofs += b->size;
	}

	return NULL;
}

void *simZeroAlloc(u32 size)
{
	void *p = simAlloc(size);
	if (p) {
		memset(p, 0, size);
	}
	return p;
}

s32 simOwns(const void *ptr)
{
	return ptr && (const u8 *)ptr >= arena && (const u8 *)ptr < arena + SIM_ARENA_SIZE;
}

void simFree(void *ptr)
{
	struct block *b;
	struct block *n;
	struct block *p;

	if (!simOwns(ptr)) {
		return;
	}

	b = (struct block *)((u8 *)ptr - HDR_SIZE);
	b->flags = 0;

	// merge with the next block
	n = blockNext(b);
	if (n && !(n->flags & FLAG_USED)) {
		struct block *nn;
		b->size += n->size;
		nn = blockNext(b);
		if (nn) {
			nn->prevsize = b->size;
		}
	}

	// merge into the previous block
	p = blockPrev(b);
	if (p && !(p->flags & FLAG_USED)) {
		struct block *nn;
		p->size += b->size;
		nn = blockNext(p);
		if (nn) {
			nn->prevsize = p->size;
		}
	}
}

void *simRealloc(void *ptr, u32 size)
{
	struct block *b;
	u32 oldsize;
	void *out;

	if (!ptr) {
		return simAlloc(size);
	}

	b = (struct block *)((u8 *)ptr - HDR_SIZE);
	oldsize = b->size - HDR_SIZE;

	if (size <= oldsize) {
		return ptr;
	}

	out = simAlloc(size);
	if (out) {
		memcpy(out, ptr, oldsize);
		simFree(ptr);
	}

	return out;
}

u8 *simArenaBase(void)
{
	return arena;
}

u32 simArenaHighWater(void)
{
	return arenaHighWater;
}

void simArenaSetHighWater(u32 hw)
{
	arenaHighWater = hw;
	arenaInit = 1;
}
