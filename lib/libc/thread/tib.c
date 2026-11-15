// THE THREAD BLOCKS
// tls data, then the tib
// the first one is made before main
#include <stdlib.h>
#include <string.h>
#include <fnu/sys.h>
#include <tib.h>

// the template, as user.ld lays out .tdata and .tbss
extern char __tls_start[], __tdata_end[], __tls_end[];
extern char __tls_align[];

static size_t tls_size, tls_align;

static size_t round_up(size_t n, size_t a) { return (n + a - 1) & ~(a - 1); }

// the block, and the tib at its top
static struct tib *tib_place(char *block) {
	struct tib *t = (struct tib *)(block + tls_size);

	memcpy(block, __tls_start, (size_t)(__tdata_end - __tls_start));
	memset(block + (__tdata_end - __tls_start), 0,
	    tls_size - (size_t)(__tdata_end - __tls_start) + sizeof(*t));
	t->__tib_self = t;
	return t;
}

struct tib *
_libc_tib_new(void)
{
	char *block;

	if ((block = aligned_alloc(tls_align,
	    round_up(tls_size + sizeof(struct tib), tls_align))) == NULL)
		return NULL;
	return tib_place(block);
}

void
_libc_tib_free(struct tib *t)
{
	free((char *)t - tls_size);
}

// malloc is not up yet
void
_libc_tib_init(void)
{
	size_t len;
	long block;
	struct tib *t;

	tls_align = (size_t)(uintptr_t)__tls_align;
	if (tls_align < _Alignof(struct tib))
		tls_align = _Alignof(struct tib);
	tls_size = round_up((size_t)(__tls_end - __tls_start), tls_align);
	len = round_up(tls_size + sizeof(struct tib), 4096);
	block = fnu_map(0, len, FNU_MEM_READ | FNU_MEM_WRITE,
	    FNU_MEM_PRIVATE | FNU_MEM_ANON, -1, 0);
	if (block < 0)
		fnu_exit(127);
	t = tib_place((char *)block);
	t->tib_tid = (pid_t)fnu_thread_id();
	fnu_thread_tcb((uint64_t)(uintptr_t)t);
}
