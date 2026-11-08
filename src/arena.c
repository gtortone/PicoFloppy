/*
 * arena.c
 * 
 * Arena-based memory allocation. Only one arena, for now.
 * 
 * Written & released by Keir Fraser <keir.xen@gmail.com>
 * 
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

/* The arena is the pico-sdk's heap region: everything between the end of
 * BSS and the end of main SRAM. Nothing in the firmware uses malloc(). */
extern char __end__[], __HeapLimit[];
#define heap_bot (__end__)
static char *heap_p;
static char *heap_top;

void *arena_alloc(uint32_t sz)
{
    void *p = heap_p;
    heap_p += (sz + 3) & ~3;
    ASSERT(heap_p <= heap_top);
    return p;
}

uint32_t arena_total(void)
{
    return heap_top - heap_bot;
}

uint32_t arena_avail(void)
{
    return heap_top - heap_p;
}

void arena_init(void)
{
    heap_p = heap_bot;
    heap_top = __HeapLimit;
}

/* The C library's heap shares its memory with the arena: make sure any
 * stray malloc() fails cleanly rather than trampling arena allocations. */
void *_sbrk(ptrdiff_t incr);
void *_sbrk(ptrdiff_t incr)
{
    return (void *)-1;
}
