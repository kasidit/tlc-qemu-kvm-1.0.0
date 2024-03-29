/*
 *  virtual page mapping and translated block handling
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include "config.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/mman.h>
#endif

// TLC
#include <pthread.h>

#include "qemu-common.h"
#include "cpu.h"
#include "cache-utils.h"

#include "tcg.h"
#include "hw/hw.h"
#include "hw/qdev.h"
#include "osdep.h"
#include "kvm.h"
#include "hw/xen.h"
#include "qemu-timer.h"
#include "memory.h"
#include "exec-memory.h"
#if defined(CONFIG_USER_ONLY)
#include <qemu.h>
#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
#include <sys/param.h>
#if __FreeBSD_version >= 700104
#define HAVE_KINFO_GETVMMAP
#define sigqueue sigqueue_freebsd  /* avoid redefinition */
#include <sys/time.h>
#include <sys/proc.h>
#include <machine/profile.h>
#define _KERNEL
#include <sys/user.h>
#undef _KERNEL
#undef sigqueue
#include <libutil.h>
#endif
#endif
#else /* !CONFIG_USER_ONLY */
#include "xen-mapcache.h"
#include "trace.h"
#endif

// TLC: begin
#include "tlc-debug.h"

#define mbarrier() __asm__ __volatile__("": : :"memory")

extern pthread_mutex_t acquire_tlc_ram_blocks; // TLC ram
// TLC end

//#define DEBUG_TB_INVALIDATE
//#define DEBUG_FLUSH
//#define DEBUG_TLB
//#define DEBUG_UNASSIGNED

/* make various TB consistency checks */
//#define DEBUG_TB_CHECK
//#define DEBUG_TLB_CHECK

//#define DEBUG_IOPORT
//#define DEBUG_SUBPAGE

#if !defined(CONFIG_USER_ONLY)
/* TB consistency checks only implemented for usermode emulation.  */
#undef DEBUG_TB_CHECK
#endif

#define SMC_BITMAP_USE_THRESHOLD 10

static TranslationBlock *tbs;
static int code_gen_max_blocks;
TranslationBlock *tb_phys_hash[CODE_GEN_PHYS_HASH_SIZE];
static int nb_tbs;
/* any access to the tbs or the page table must use this lock */
spinlock_t tb_lock = SPIN_LOCK_UNLOCKED;

#if defined(__arm__) || defined(__sparc_v9__)
/* The prologue must be reachable with a direct jump. ARM and Sparc64
 have limited branch ranges (possibly also PPC) so place it in a
 section close to code segment. */
#define code_gen_section                                \
    __attribute__((__section__(".gen_code")))           \
    __attribute__((aligned (32)))
#elif defined(_WIN32)
/* Maximum alignment for Win32 is 16. */
#define code_gen_section                                \
    __attribute__((aligned (16)))
#else
#define code_gen_section                                \
    __attribute__((aligned (32)))
#endif

uint8_t code_gen_prologue[1024] code_gen_section;
static uint8_t *code_gen_buffer;
static unsigned long code_gen_buffer_size;
/* threshold to flush the translated code buffer */
static unsigned long code_gen_buffer_max_size;
static uint8_t *code_gen_ptr;

#if !defined(CONFIG_USER_ONLY)
int phys_ram_fd;
static int in_migration;

RAMList ram_list = { .blocks = QLIST_HEAD_INITIALIZER(ram_list.blocks) };
RAMList tlc_ram_list = { .blocks = QLIST_HEAD_INITIALIZER(tlc_ram_list.blocks) }; // TLC ram

static MemoryRegion *system_memory;
static MemoryRegion *system_io;

#endif

CPUState *first_cpu;
/* current CPU in the current thread. It is only valid inside
   cpu_exec() */
DEFINE_TLS(CPUState *,cpu_single_env);
/* 0 = Do not count executed instructions.
   1 = Precise instruction counting.
   2 = Adaptive rate instruction counting.  */
int use_icount = 0;

typedef struct PageDesc {
    /* list of TBs intersecting this ram page */
    TranslationBlock *first_tb;
    /* in order to optimize self modifying code, we count the number
       of lookups we do to a given page to use a bitmap */
    unsigned int code_write_count;
    uint8_t *code_bitmap;
#if defined(CONFIG_USER_ONLY)
    unsigned long flags;
#endif
} PageDesc;

/* In system mode we want L1_MAP to be based on ram offsets,
   while in user mode we want it to be based on virtual addresses.  */
#if !defined(CONFIG_USER_ONLY)
#if HOST_LONG_BITS < TARGET_PHYS_ADDR_SPACE_BITS
# define L1_MAP_ADDR_SPACE_BITS  HOST_LONG_BITS
#else
# define L1_MAP_ADDR_SPACE_BITS  TARGET_PHYS_ADDR_SPACE_BITS
#endif
#else
# define L1_MAP_ADDR_SPACE_BITS  TARGET_VIRT_ADDR_SPACE_BITS
#endif

/* Size of the L2 (and L3, etc) page tables.  */
#define L2_BITS 10
#define L2_SIZE (1 << L2_BITS)

/* The bits remaining after N lower levels of page tables.  */
#define P_L1_BITS_REM \
    ((TARGET_PHYS_ADDR_SPACE_BITS - TARGET_PAGE_BITS) % L2_BITS)
#define V_L1_BITS_REM \
    ((L1_MAP_ADDR_SPACE_BITS - TARGET_PAGE_BITS) % L2_BITS)

/* Size of the L1 page table.  Avoid silly small sizes.  */
#if P_L1_BITS_REM < 4
#define P_L1_BITS  (P_L1_BITS_REM + L2_BITS)
#else
#define P_L1_BITS  P_L1_BITS_REM
#endif

#if V_L1_BITS_REM < 4
#define V_L1_BITS  (V_L1_BITS_REM + L2_BITS)
#else
#define V_L1_BITS  V_L1_BITS_REM
#endif

#define P_L1_SIZE  ((target_phys_addr_t)1 << P_L1_BITS)
#define V_L1_SIZE  ((target_ulong)1 << V_L1_BITS)

#define P_L1_SHIFT (TARGET_PHYS_ADDR_SPACE_BITS - TARGET_PAGE_BITS - P_L1_BITS)
#define V_L1_SHIFT (L1_MAP_ADDR_SPACE_BITS - TARGET_PAGE_BITS - V_L1_BITS)

unsigned long qemu_real_host_page_size;
unsigned long qemu_host_page_size;
unsigned long qemu_host_page_mask;

/* This is a multi-level map on the virtual address space.
   The bottom level has pointers to PageDesc.  */
static void *l1_map[V_L1_SIZE];

#if !defined(CONFIG_USER_ONLY)
typedef struct PhysPageDesc {
    /* offset in host memory of the page + io_index in the low bits */
    ram_addr_t phys_offset;
    ram_addr_t region_offset;
} PhysPageDesc;

/* This is a multi-level map on the physical address space.
   The bottom level has pointers to PhysPageDesc.  */
static void *l1_phys_map[P_L1_SIZE];

static void io_mem_init(void);
static void memory_map_init(void);

/* io memory support */
CPUWriteMemoryFunc *io_mem_write[IO_MEM_NB_ENTRIES][4];
CPUReadMemoryFunc *io_mem_read[IO_MEM_NB_ENTRIES][4];
void *io_mem_opaque[IO_MEM_NB_ENTRIES];
static char io_mem_used[IO_MEM_NB_ENTRIES];
static int io_mem_watch;
#endif

/* log support */
#ifdef WIN32
static const char *logfilename = "qemu.log";
#else
static const char *logfilename = "/tmp/qemu.log";
#endif
FILE *logfile;
int loglevel;
static int log_append = 0;

/* statistics */
#if !defined(CONFIG_USER_ONLY)
static int tlb_flush_count;
#endif
static int tb_flush_count;
static int tb_phys_invalidate_count;

#ifdef _WIN32
static void map_exec(void *addr, long size)
{
    DWORD old_protect;
    VirtualProtect(addr, size,
                   PAGE_EXECUTE_READWRITE, &old_protect);
    
}
#else
static void map_exec(void *addr, long size)
{
    unsigned long start, end, page_size;
    
    page_size = getpagesize();
    start = (unsigned long)addr;
    start &= ~(page_size - 1);
    
    end = (unsigned long)addr + size;
    end += page_size - 1;
    end &= ~(page_size - 1);
    
    mprotect((void *)start, end - start,
             PROT_READ | PROT_WRITE | PROT_EXEC);
}
#endif

static void page_init(void)
{
    /* NOTE: we can always suppose that qemu_host_page_size >=
       TARGET_PAGE_SIZE */
#ifdef _WIN32
    {
        SYSTEM_INFO system_info;

        GetSystemInfo(&system_info);
        qemu_real_host_page_size = system_info.dwPageSize;
    }
#else
    qemu_real_host_page_size = getpagesize();
#endif
    if (qemu_host_page_size == 0)
        qemu_host_page_size = qemu_real_host_page_size;
    if (qemu_host_page_size < TARGET_PAGE_SIZE)
        qemu_host_page_size = TARGET_PAGE_SIZE;
    qemu_host_page_mask = ~(qemu_host_page_size - 1);

#if defined(CONFIG_BSD) && defined(CONFIG_USER_ONLY)
    {
#ifdef HAVE_KINFO_GETVMMAP
        struct kinfo_vmentry *freep;
        int i, cnt;

        freep = kinfo_getvmmap(getpid(), &cnt);
        if (freep) {
            mmap_lock();
            for (i = 0; i < cnt; i++) {
                unsigned long startaddr, endaddr;

                startaddr = freep[i].kve_start;
                endaddr = freep[i].kve_end;
                if (h2g_valid(startaddr)) {
                    startaddr = h2g(startaddr) & TARGET_PAGE_MASK;

                    if (h2g_valid(endaddr)) {
                        endaddr = h2g(endaddr);
                        page_set_flags(startaddr, endaddr, PAGE_RESERVED);
                    } else {
#if TARGET_ABI_BITS <= L1_MAP_ADDR_SPACE_BITS
                        endaddr = ~0ul;
                        page_set_flags(startaddr, endaddr, PAGE_RESERVED);
#endif
                    }
                }
            }
            free(freep);
            mmap_unlock();
        }
#else
        FILE *f;

        last_brk = (unsigned long)sbrk(0);

        f = fopen("/compat/linux/proc/self/maps", "r");
        if (f) {
            mmap_lock();

            do {
                unsigned long startaddr, endaddr;
                int n;

                n = fscanf (f, "%lx-%lx %*[^\n]\n", &startaddr, &endaddr);

                if (n == 2 && h2g_valid(startaddr)) {
                    startaddr = h2g(startaddr) & TARGET_PAGE_MASK;

                    if (h2g_valid(endaddr)) {
                        endaddr = h2g(endaddr);
                    } else {
                        endaddr = ~0ul;
                    }
                    page_set_flags(startaddr, endaddr, PAGE_RESERVED);
                }
            } while (!feof(f));

            fclose(f);
            mmap_unlock();
        }
#endif
    }
#endif
}

static PageDesc *page_find_alloc(tb_page_addr_t index, int alloc)
{
    PageDesc *pd;
    void **lp;
    int i;

#if defined(CONFIG_USER_ONLY)
    /* We can't use g_malloc because it may recurse into a locked mutex. */
# define ALLOC(P, SIZE)                                 \
    do {                                                \
        P = mmap(NULL, SIZE, PROT_READ | PROT_WRITE,    \
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);   \
    } while (0)
#else
# define ALLOC(P, SIZE) \
    do { P = g_malloc0(SIZE); } while (0)
#endif

    /* Level 1.  Always allocated.  */
    lp = l1_map + ((index >> V_L1_SHIFT) & (V_L1_SIZE - 1));

    /* Level 2..N-1.  */
    for (i = V_L1_SHIFT / L2_BITS - 1; i > 0; i--) {
        void **p = *lp;

        if (p == NULL) {
            if (!alloc) {
                return NULL;
            }
            ALLOC(p, sizeof(void *) * L2_SIZE);
            *lp = p;
        }

        lp = p + ((index >> (i * L2_BITS)) & (L2_SIZE - 1));
    }

    pd = *lp;
    if (pd == NULL) {
        if (!alloc) {
            return NULL;
        }
        ALLOC(pd, sizeof(PageDesc) * L2_SIZE);
        *lp = pd;
    }

#undef ALLOC

    return pd + (index & (L2_SIZE - 1));
}

static inline PageDesc *page_find(tb_page_addr_t index)
{
    return page_find_alloc(index, 0);
}

#if !defined(CONFIG_USER_ONLY)
static PhysPageDesc *phys_page_find_alloc(target_phys_addr_t index, int alloc)
{
    PhysPageDesc *pd;
    void **lp;
    int i;

    /* Level 1.  Always allocated.  */
    lp = l1_phys_map + ((index >> P_L1_SHIFT) & (P_L1_SIZE - 1));

    /* Level 2..N-1.  */
    for (i = P_L1_SHIFT / L2_BITS - 1; i > 0; i--) {
        void **p = *lp;
        if (p == NULL) {
            if (!alloc) {
                return NULL;
            }
            *lp = p = g_malloc0(sizeof(void *) * L2_SIZE);
        }
        lp = p + ((index >> (i * L2_BITS)) & (L2_SIZE - 1));
    }

    pd = *lp;
    if (pd == NULL) {
        int i;

        if (!alloc) {
            return NULL;
        }

        *lp = pd = g_malloc(sizeof(PhysPageDesc) * L2_SIZE);

        for (i = 0; i < L2_SIZE; i++) {
            pd[i].phys_offset = IO_MEM_UNASSIGNED;
            pd[i].region_offset = (index + i) << TARGET_PAGE_BITS;
        }
    }

    return pd + (index & (L2_SIZE - 1));
}

static inline PhysPageDesc *phys_page_find(target_phys_addr_t index)
{
    return phys_page_find_alloc(index, 0);
}

static void tlb_protect_code(ram_addr_t ram_addr);
static void tlb_unprotect_code_phys(CPUState *env, ram_addr_t ram_addr,
                                    target_ulong vaddr);
#define mmap_lock() do { } while(0)
#define mmap_unlock() do { } while(0)
#endif

#define DEFAULT_CODE_GEN_BUFFER_SIZE (32 * 1024 * 1024)

#if defined(CONFIG_USER_ONLY)
/* Currently it is not recommended to allocate big chunks of data in
   user mode. It will change when a dedicated libc will be used */
#define USE_STATIC_CODE_GEN_BUFFER
#endif

#ifdef USE_STATIC_CODE_GEN_BUFFER
static uint8_t static_code_gen_buffer[DEFAULT_CODE_GEN_BUFFER_SIZE]
               __attribute__((aligned (CODE_GEN_ALIGN)));
#endif

static void code_gen_alloc(unsigned long tb_size)
{
#ifdef USE_STATIC_CODE_GEN_BUFFER
    code_gen_buffer = static_code_gen_buffer;
    code_gen_buffer_size = DEFAULT_CODE_GEN_BUFFER_SIZE;
    map_exec(code_gen_buffer, code_gen_buffer_size);
#else
    code_gen_buffer_size = tb_size;
    if (code_gen_buffer_size == 0) {
#if defined(CONFIG_USER_ONLY)
        code_gen_buffer_size = DEFAULT_CODE_GEN_BUFFER_SIZE;
#else
        /* XXX: needs adjustments */
        code_gen_buffer_size = (unsigned long)(ram_size / 4);
#endif
    }
    if (code_gen_buffer_size < MIN_CODE_GEN_BUFFER_SIZE)
        code_gen_buffer_size = MIN_CODE_GEN_BUFFER_SIZE;
    /* The code gen buffer location may have constraints depending on
       the host cpu and OS */
#if defined(__linux__) 
    {
        int flags;
        void *start = NULL;

        flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(__x86_64__)
        flags |= MAP_32BIT;
        /* Cannot map more than that */
        if (code_gen_buffer_size > (800 * 1024 * 1024))
            code_gen_buffer_size = (800 * 1024 * 1024);
#elif defined(__sparc_v9__)
        // Map the buffer below 2G, so we can use direct calls and branches
        flags |= MAP_FIXED;
        start = (void *) 0x60000000UL;
        if (code_gen_buffer_size > (512 * 1024 * 1024))
            code_gen_buffer_size = (512 * 1024 * 1024);
#elif defined(__arm__)
        /* Map the buffer below 32M, so we can use direct calls and branches */
        flags |= MAP_FIXED;
        start = (void *) 0x01000000UL;
        if (code_gen_buffer_size > 16 * 1024 * 1024)
            code_gen_buffer_size = 16 * 1024 * 1024;
#elif defined(__s390x__)
        /* Map the buffer so that we can use direct calls and branches.  */
        /* We have a +- 4GB range on the branches; leave some slop.  */
        if (code_gen_buffer_size > (3ul * 1024 * 1024 * 1024)) {
            code_gen_buffer_size = 3ul * 1024 * 1024 * 1024;
        }
        start = (void *)0x90000000UL;
#endif
        code_gen_buffer = mmap(start, code_gen_buffer_size,
                               PROT_WRITE | PROT_READ | PROT_EXEC,
                               flags, -1, 0);
        if (code_gen_buffer == MAP_FAILED) {
            fprintf(stderr, "Could not allocate dynamic translator buffer\n");
            exit(1);
        }
    }
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__) \
    || defined(__DragonFly__) || defined(__OpenBSD__) \
    || defined(__NetBSD__)
    {
        int flags;
        void *addr = NULL;
        flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(__x86_64__)
        /* FreeBSD doesn't have MAP_32BIT, use MAP_FIXED and assume
         * 0x40000000 is free */
        flags |= MAP_FIXED;
        addr = (void *)0x40000000;
        /* Cannot map more than that */
        if (code_gen_buffer_size > (800 * 1024 * 1024))
            code_gen_buffer_size = (800 * 1024 * 1024);
#elif defined(__sparc_v9__)
        // Map the buffer below 2G, so we can use direct calls and branches
        flags |= MAP_FIXED;
        addr = (void *) 0x60000000UL;
        if (code_gen_buffer_size > (512 * 1024 * 1024)) {
            code_gen_buffer_size = (512 * 1024 * 1024);
        }
#endif
        code_gen_buffer = mmap(addr, code_gen_buffer_size,
                               PROT_WRITE | PROT_READ | PROT_EXEC, 
                               flags, -1, 0);
        if (code_gen_buffer == MAP_FAILED) {
            fprintf(stderr, "Could not allocate dynamic translator buffer\n");
            exit(1);
        }
    }
#else
    code_gen_buffer = g_malloc(code_gen_buffer_size);
    map_exec(code_gen_buffer, code_gen_buffer_size);
#endif
#endif /* !USE_STATIC_CODE_GEN_BUFFER */
    map_exec(code_gen_prologue, sizeof(code_gen_prologue));
    code_gen_buffer_max_size = code_gen_buffer_size -
        (TCG_MAX_OP_SIZE * OPC_BUF_SIZE);
    code_gen_max_blocks = code_gen_buffer_size / CODE_GEN_AVG_BLOCK_SIZE;
    tbs = g_malloc(code_gen_max_blocks * sizeof(TranslationBlock));
}

/* Must be called before using the QEMU cpus. 'tb_size' is the size
   (in bytes) allocated to the translation buffer. Zero means default
   size. */
void tcg_exec_init(unsigned long tb_size)
{
    cpu_gen_init();
    code_gen_alloc(tb_size);
    code_gen_ptr = code_gen_buffer;
    page_init();
#if !defined(CONFIG_USER_ONLY) || !defined(CONFIG_USE_GUEST_BASE)
    /* There's no guest base to take into account, so go ahead and
       initialize the prologue now.  */
    tcg_prologue_init(&tcg_ctx);
#endif
}

bool tcg_enabled(void)
{
    return code_gen_buffer != NULL;
}

void cpu_exec_init_all(void)
{
#if !defined(CONFIG_USER_ONLY)
    memory_map_init();
    io_mem_init();
#endif
}

#if defined(CPU_SAVE_VERSION) && !defined(CONFIG_USER_ONLY)

static int cpu_common_post_load(void *opaque, int version_id)
{
    CPUState *env = opaque;

    /* 0x01 was CPU_INTERRUPT_EXIT. This line can be removed when the
       version_id is increased. */
    env->interrupt_request &= ~0x01;
    tlb_flush(env, 1);

    return 0;
}

static const VMStateDescription vmstate_cpu_common = {
    .name = "cpu_common",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .post_load = cpu_common_post_load,
    .fields      = (VMStateField []) {
        VMSTATE_UINT32(halted, CPUState),
        VMSTATE_UINT32(interrupt_request, CPUState),
        VMSTATE_END_OF_LIST()
    }
};
#endif

CPUState *qemu_get_cpu(int cpu)
{
    CPUState *env = first_cpu;

    while (env) {
        if (env->cpu_index == cpu)
            break;
        env = env->next_cpu;
    }

    return env;
}

void cpu_exec_init(CPUState *env)
{
    CPUState **penv;
    int cpu_index;

#if defined(CONFIG_USER_ONLY)
    cpu_list_lock();
#endif
    env->next_cpu = NULL;
    penv = &first_cpu;
    cpu_index = 0;
    while (*penv != NULL) {
        penv = &(*penv)->next_cpu;
        cpu_index++;
    }
    env->cpu_index = cpu_index;
    env->numa_node = 0;
    QTAILQ_INIT(&env->breakpoints);
    QTAILQ_INIT(&env->watchpoints);
#ifndef CONFIG_USER_ONLY
    env->thread_id = qemu_get_thread_id();
#endif
    *penv = env;
#if defined(CONFIG_USER_ONLY)
    cpu_list_unlock();
#endif
#if defined(CPU_SAVE_VERSION) && !defined(CONFIG_USER_ONLY)
    vmstate_register(NULL, cpu_index, &vmstate_cpu_common, env);
    register_savevm(NULL, "cpu", cpu_index, CPU_SAVE_VERSION,
                    cpu_save, cpu_load, env);
#endif
}

/* Allocate a new translation block. Flush the translation buffer if
   too many translation blocks or too much generated code. */
static TranslationBlock *tb_alloc(target_ulong pc)
{
    TranslationBlock *tb;

    if (nb_tbs >= code_gen_max_blocks ||
        (code_gen_ptr - code_gen_buffer) >= code_gen_buffer_max_size)
        return NULL;
    tb = &tbs[nb_tbs++];
    tb->pc = pc;
    tb->cflags = 0;
    return tb;
}

void tb_free(TranslationBlock *tb)
{
    /* In practice this is mostly used for single use temporary TB
       Ignore the hard cases and just back up if this TB happens to
       be the last one generated.  */
    if (nb_tbs > 0 && tb == &tbs[nb_tbs - 1]) {
        code_gen_ptr = tb->tc_ptr;
        nb_tbs--;
    }
}

static inline void invalidate_page_bitmap(PageDesc *p)
{
    if (p->code_bitmap) {
        g_free(p->code_bitmap);
        p->code_bitmap = NULL;
    }
    p->code_write_count = 0;
}

/* Set to NULL all the 'first_tb' fields in all PageDescs. */

static void page_flush_tb_1 (int level, void **lp)
{
    int i;

    if (*lp == NULL) {
        return;
    }
    if (level == 0) {
        PageDesc *pd = *lp;
        for (i = 0; i < L2_SIZE; ++i) {
            pd[i].first_tb = NULL;
            invalidate_page_bitmap(pd + i);
        }
    } else {
        void **pp = *lp;
        for (i = 0; i < L2_SIZE; ++i) {
            page_flush_tb_1 (level - 1, pp + i);
        }
    }
}

static void page_flush_tb(void)
{
    int i;
    for (i = 0; i < V_L1_SIZE; i++) {
        page_flush_tb_1(V_L1_SHIFT / L2_BITS - 1, l1_map + i);
    }
}

/* flush all the translation blocks */
/* XXX: tb_flush is currently not thread safe */
void tb_flush(CPUState *env1)
{
    CPUState *env;
#if defined(DEBUG_FLUSH)
    printf("qemu: flush code_size=%ld nb_tbs=%d avg_tb_size=%ld\n",
           (unsigned long)(code_gen_ptr - code_gen_buffer),
           nb_tbs, nb_tbs > 0 ?
           ((unsigned long)(code_gen_ptr - code_gen_buffer)) / nb_tbs : 0);
#endif
    if ((unsigned long)(code_gen_ptr - code_gen_buffer) > code_gen_buffer_size)
        cpu_abort(env1, "Internal error: code buffer overflow\n");

    nb_tbs = 0;

    for(env = first_cpu; env != NULL; env = env->next_cpu) {
        memset (env->tb_jmp_cache, 0, TB_JMP_CACHE_SIZE * sizeof (void *));
    }

    memset (tb_phys_hash, 0, CODE_GEN_PHYS_HASH_SIZE * sizeof (void *));
    page_flush_tb();

    code_gen_ptr = code_gen_buffer;
    /* XXX: flush processor icache at this point if cache flush is
       expensive */
    tb_flush_count++;
}

#ifdef DEBUG_TB_CHECK

static void tb_invalidate_check(target_ulong address)
{
    TranslationBlock *tb;
    int i;
    address &= TARGET_PAGE_MASK;
    for(i = 0;i < CODE_GEN_PHYS_HASH_SIZE; i++) {
        for(tb = tb_phys_hash[i]; tb != NULL; tb = tb->phys_hash_next) {
            if (!(address + TARGET_PAGE_SIZE <= tb->pc ||
                  address >= tb->pc + tb->size)) {
                printf("ERROR invalidate: address=" TARGET_FMT_lx
                       " PC=%08lx size=%04x\n",
                       address, (long)tb->pc, tb->size);
            }
        }
    }
}

/* verify that all the pages have correct rights for code */
static void tb_page_check(void)
{
    TranslationBlock *tb;
    int i, flags1, flags2;

    for(i = 0;i < CODE_GEN_PHYS_HASH_SIZE; i++) {
        for(tb = tb_phys_hash[i]; tb != NULL; tb = tb->phys_hash_next) {
            flags1 = page_get_flags(tb->pc);
            flags2 = page_get_flags(tb->pc + tb->size - 1);
            if ((flags1 & PAGE_WRITE) || (flags2 & PAGE_WRITE)) {
                printf("ERROR page flags: PC=%08lx size=%04x f1=%x f2=%x\n",
                       (long)tb->pc, tb->size, flags1, flags2);
            }
        }
    }
}

#endif

/* invalidate one TB */
static inline void tb_remove(TranslationBlock **ptb, TranslationBlock *tb,
                             int next_offset)
{
    TranslationBlock *tb1;
    for(;;) {
        tb1 = *ptb;
        if (tb1 == tb) {
            *ptb = *(TranslationBlock **)((char *)tb1 + next_offset);
            break;
        }
        ptb = (TranslationBlock **)((char *)tb1 + next_offset);
    }
}

static inline void tb_page_remove(TranslationBlock **ptb, TranslationBlock *tb)
{
    TranslationBlock *tb1;
    unsigned int n1;

    for(;;) {
        tb1 = *ptb;
        n1 = (long)tb1 & 3;
        tb1 = (TranslationBlock *)((long)tb1 & ~3);
        if (tb1 == tb) {
            *ptb = tb1->page_next[n1];
            break;
        }
        ptb = &tb1->page_next[n1];
    }
}

static inline void tb_jmp_remove(TranslationBlock *tb, int n)
{
    TranslationBlock *tb1, **ptb;
    unsigned int n1;

    ptb = &tb->jmp_next[n];
    tb1 = *ptb;
    if (tb1) {
        /* find tb(n) in circular list */
        for(;;) {
            tb1 = *ptb;
            n1 = (long)tb1 & 3;
            tb1 = (TranslationBlock *)((long)tb1 & ~3);
            if (n1 == n && tb1 == tb)
                break;
            if (n1 == 2) {
                ptb = &tb1->jmp_first;
            } else {
                ptb = &tb1->jmp_next[n1];
            }
        }
        /* now we can suppress tb(n) from the list */
        *ptb = tb->jmp_next[n];

        tb->jmp_next[n] = NULL;
    }
}

/* reset the jump entry 'n' of a TB so that it is not chained to
   another TB */
static inline void tb_reset_jump(TranslationBlock *tb, int n)
{
    tb_set_jmp_target(tb, n, (unsigned long)(tb->tc_ptr + tb->tb_next_offset[n]));
}

void tb_phys_invalidate(TranslationBlock *tb, tb_page_addr_t page_addr)
{
    CPUState *env;
    PageDesc *p;
    unsigned int h, n1;
    tb_page_addr_t phys_pc;
    TranslationBlock *tb1, *tb2;

    /* remove the TB from the hash list */
    phys_pc = tb->page_addr[0] + (tb->pc & ~TARGET_PAGE_MASK);
    h = tb_phys_hash_func(phys_pc);
    tb_remove(&tb_phys_hash[h], tb,
              offsetof(TranslationBlock, phys_hash_next));

    /* remove the TB from the page list */
    if (tb->page_addr[0] != page_addr) {
        p = page_find(tb->page_addr[0] >> TARGET_PAGE_BITS);
        tb_page_remove(&p->first_tb, tb);
        invalidate_page_bitmap(p);
    }
    if (tb->page_addr[1] != -1 && tb->page_addr[1] != page_addr) {
        p = page_find(tb->page_addr[1] >> TARGET_PAGE_BITS);
        tb_page_remove(&p->first_tb, tb);
        invalidate_page_bitmap(p);
    }

    tb_invalidated_flag = 1;

    /* remove the TB from the hash list */
    h = tb_jmp_cache_hash_func(tb->pc);
    for(env = first_cpu; env != NULL; env = env->next_cpu) {
        if (env->tb_jmp_cache[h] == tb)
            env->tb_jmp_cache[h] = NULL;
    }

    /* suppress this TB from the two jump lists */
    tb_jmp_remove(tb, 0);
    tb_jmp_remove(tb, 1);

    /* suppress any remaining jumps to this TB */
    tb1 = tb->jmp_first;
    for(;;) {
        n1 = (long)tb1 & 3;
        if (n1 == 2)
            break;
        tb1 = (TranslationBlock *)((long)tb1 & ~3);
        tb2 = tb1->jmp_next[n1];
        tb_reset_jump(tb1, n1);
        tb1->jmp_next[n1] = NULL;
        tb1 = tb2;
    }
    tb->jmp_first = (TranslationBlock *)((long)tb | 2); /* fail safe */

    tb_phys_invalidate_count++;
}

static inline void set_bits(uint8_t *tab, int start, int len)
{
    int end, mask, end1;

    end = start + len;
    tab += start >> 3;
    mask = 0xff << (start & 7);
    if ((start & ~7) == (end & ~7)) {
        if (start < end) {
            mask &= ~(0xff << (end & 7));
            *tab |= mask;
        }
    } else {
        *tab++ |= mask;
        start = (start + 8) & ~7;
        end1 = end & ~7;
        while (start < end1) {
            *tab++ = 0xff;
            start += 8;
        }
        if (start < end) {
            mask = ~(0xff << (end & 7));
            *tab |= mask;
        }
    }
}

static void build_page_bitmap(PageDesc *p)
{
    int n, tb_start, tb_end;
    TranslationBlock *tb;

    p->code_bitmap = g_malloc0(TARGET_PAGE_SIZE / 8);

    tb = p->first_tb;
    while (tb != NULL) {
        n = (long)tb & 3;
        tb = (TranslationBlock *)((long)tb & ~3);
        /* NOTE: this is subtle as a TB may span two physical pages */
        if (n == 0) {
            /* NOTE: tb_end may be after the end of the page, but
               it is not a problem */
            tb_start = tb->pc & ~TARGET_PAGE_MASK;
            tb_end = tb_start + tb->size;
            if (tb_end > TARGET_PAGE_SIZE)
                tb_end = TARGET_PAGE_SIZE;
        } else {
            tb_start = 0;
            tb_end = ((tb->pc + tb->size) & ~TARGET_PAGE_MASK);
        }
        set_bits(p->code_bitmap, tb_start, tb_end - tb_start);
        tb = tb->page_next[n];
    }
}

TranslationBlock *tb_gen_code(CPUState *env,
                              target_ulong pc, target_ulong cs_base,
                              int flags, int cflags)
{
    TranslationBlock *tb;
    uint8_t *tc_ptr;
    tb_page_addr_t phys_pc, phys_page2;
    target_ulong virt_page2;
    int code_gen_size;

    phys_pc = get_page_addr_code(env, pc);
    tb = tb_alloc(pc);
    if (!tb) {
        /* flush must be done */
        tb_flush(env);
        /* cannot fail at this point */
        tb = tb_alloc(pc);
        /* Don't forget to invalidate previous TB info.  */
        tb_invalidated_flag = 1;
    }
    tc_ptr = code_gen_ptr;
    tb->tc_ptr = tc_ptr;
    tb->cs_base = cs_base;
    tb->flags = flags;
    tb->cflags = cflags;
    cpu_gen_code(env, tb, &code_gen_size);
    code_gen_ptr = (void *)(((unsigned long)code_gen_ptr + code_gen_size + CODE_GEN_ALIGN - 1) & ~(CODE_GEN_ALIGN - 1));

    /* check next page if needed */
    virt_page2 = (pc + tb->size - 1) & TARGET_PAGE_MASK;
    phys_page2 = -1;
    if ((pc & TARGET_PAGE_MASK) != virt_page2) {
        phys_page2 = get_page_addr_code(env, virt_page2);
    }
    tb_link_page(tb, phys_pc, phys_page2);
    return tb;
}

/* invalidate all TBs which intersect with the target physical page
   starting in range [start;end[. NOTE: start and end must refer to
   the same physical page. 'is_cpu_write_access' should be true if called
   from a real cpu write access: the virtual CPU will exit the current
   TB if code is modified inside this TB. */
void tb_invalidate_phys_page_range(tb_page_addr_t start, tb_page_addr_t end,
                                   int is_cpu_write_access)
{
    TranslationBlock *tb, *tb_next, *saved_tb;
    CPUState *env = cpu_single_env;
    tb_page_addr_t tb_start, tb_end;
    PageDesc *p;
    int n;
#ifdef TARGET_HAS_PRECISE_SMC
    int current_tb_not_found = is_cpu_write_access;
    TranslationBlock *current_tb = NULL;
    int current_tb_modified = 0;
    target_ulong current_pc = 0;
    target_ulong current_cs_base = 0;
    int current_flags = 0;
#endif /* TARGET_HAS_PRECISE_SMC */

    p = page_find(start >> TARGET_PAGE_BITS);
    if (!p)
        return;
    if (!p->code_bitmap &&
        ++p->code_write_count >= SMC_BITMAP_USE_THRESHOLD &&
        is_cpu_write_access) {
        /* build code bitmap */
        build_page_bitmap(p);
    }

    /* we remove all the TBs in the range [start, end[ */
    /* XXX: see if in some cases it could be faster to invalidate all the code */
    tb = p->first_tb;
    while (tb != NULL) {
        n = (long)tb & 3;
        tb = (TranslationBlock *)((long)tb & ~3);
        tb_next = tb->page_next[n];
        /* NOTE: this is subtle as a TB may span two physical pages */
        if (n == 0) {
            /* NOTE: tb_end may be after the end of the page, but
               it is not a problem */
            tb_start = tb->page_addr[0] + (tb->pc & ~TARGET_PAGE_MASK);
            tb_end = tb_start + tb->size;
        } else {
            tb_start = tb->page_addr[1];
            tb_end = tb_start + ((tb->pc + tb->size) & ~TARGET_PAGE_MASK);
        }
        if (!(tb_end <= start || tb_start >= end)) {
#ifdef TARGET_HAS_PRECISE_SMC
            if (current_tb_not_found) {
                current_tb_not_found = 0;
                current_tb = NULL;
                if (env->mem_io_pc) {
                    /* now we have a real cpu fault */
                    current_tb = tb_find_pc(env->mem_io_pc);
                }
            }
            if (current_tb == tb &&
                (current_tb->cflags & CF_COUNT_MASK) != 1) {
                /* If we are modifying the current TB, we must stop
                its execution. We could be more precise by checking
                that the modification is after the current PC, but it
                would require a specialized function to partially
                restore the CPU state */

                current_tb_modified = 1;
                cpu_restore_state(current_tb, env, env->mem_io_pc);
                cpu_get_tb_cpu_state(env, &current_pc, &current_cs_base,
                                     &current_flags);
            }
#endif /* TARGET_HAS_PRECISE_SMC */
            /* we need to do that to handle the case where a signal
               occurs while doing tb_phys_invalidate() */
            saved_tb = NULL;
            if (env) {
                saved_tb = env->current_tb;
                env->current_tb = NULL;
            }
            tb_phys_invalidate(tb, -1);
            if (env) {
                env->current_tb = saved_tb;
                if (env->interrupt_request && env->current_tb)
                    cpu_interrupt(env, env->interrupt_request);
            }
        }
        tb = tb_next;
    }
#if !defined(CONFIG_USER_ONLY)
    /* if no code remaining, no need to continue to use slow writes */
    if (!p->first_tb) {
        invalidate_page_bitmap(p);
        if (is_cpu_write_access) {
            tlb_unprotect_code_phys(env, start, env->mem_io_vaddr);
        }
    }
#endif
#ifdef TARGET_HAS_PRECISE_SMC
    if (current_tb_modified) {
        /* we generate a block containing just the instruction
           modifying the memory. It will ensure that it cannot modify
           itself */
        env->current_tb = NULL;
        tb_gen_code(env, current_pc, current_cs_base, current_flags, 1);
        cpu_resume_from_signal(env, NULL);
    }
#endif
}

/* len must be <= 8 and start must be a multiple of len */
static inline void tb_invalidate_phys_page_fast(tb_page_addr_t start, int len)
{
    PageDesc *p;
    int offset, b;
#if 0
    if (1) {
        qemu_log("modifying code at 0x%x size=%d EIP=%x PC=%08x\n",
                  cpu_single_env->mem_io_vaddr, len,
                  cpu_single_env->eip,
                  cpu_single_env->eip + (long)cpu_single_env->segs[R_CS].base);
    }
#endif
    p = page_find(start >> TARGET_PAGE_BITS);
    if (!p)
        return;
    if (p->code_bitmap) {
        offset = start & ~TARGET_PAGE_MASK;
        b = p->code_bitmap[offset >> 3] >> (offset & 7);
        if (b & ((1 << len) - 1))
            goto do_invalidate;
    } else {
    do_invalidate:
        tb_invalidate_phys_page_range(start, start + len, 1);
    }
}

#if !defined(CONFIG_SOFTMMU)
static void tb_invalidate_phys_page(tb_page_addr_t addr,
                                    unsigned long pc, void *puc)
{
    TranslationBlock *tb;
    PageDesc *p;
    int n;
#ifdef TARGET_HAS_PRECISE_SMC
    TranslationBlock *current_tb = NULL;
    CPUState *env = cpu_single_env;
    int current_tb_modified = 0;
    target_ulong current_pc = 0;
    target_ulong current_cs_base = 0;
    int current_flags = 0;
#endif

    addr &= TARGET_PAGE_MASK;
    p = page_find(addr >> TARGET_PAGE_BITS);
    if (!p)
        return;
    tb = p->first_tb;
#ifdef TARGET_HAS_PRECISE_SMC
    if (tb && pc != 0) {
        current_tb = tb_find_pc(pc);
    }
#endif
    while (tb != NULL) {
        n = (long)tb & 3;
        tb = (TranslationBlock *)((long)tb & ~3);
#ifdef TARGET_HAS_PRECISE_SMC
        if (current_tb == tb &&
            (current_tb->cflags & CF_COUNT_MASK) != 1) {
                /* If we are modifying the current TB, we must stop
                   its execution. We could be more precise by checking
                   that the modification is after the current PC, but it
                   would require a specialized function to partially
                   restore the CPU state */

            current_tb_modified = 1;
            cpu_restore_state(current_tb, env, pc);
            cpu_get_tb_cpu_state(env, &current_pc, &current_cs_base,
                                 &current_flags);
        }
#endif /* TARGET_HAS_PRECISE_SMC */
        tb_phys_invalidate(tb, addr);
        tb = tb->page_next[n];
    }
    p->first_tb = NULL;
#ifdef TARGET_HAS_PRECISE_SMC
    if (current_tb_modified) {
        /* we generate a block containing just the instruction
           modifying the memory. It will ensure that it cannot modify
           itself */
        env->current_tb = NULL;
        tb_gen_code(env, current_pc, current_cs_base, current_flags, 1);
        cpu_resume_from_signal(env, puc);
    }
#endif
}
#endif

/* add the tb in the target page and protect it if necessary */
static inline void tb_alloc_page(TranslationBlock *tb,
                                 unsigned int n, tb_page_addr_t page_addr)
{
    PageDesc *p;
#ifndef CONFIG_USER_ONLY
    bool page_already_protected;
#endif

    tb->page_addr[n] = page_addr;
    p = page_find_alloc(page_addr >> TARGET_PAGE_BITS, 1);
    tb->page_next[n] = p->first_tb;
#ifndef CONFIG_USER_ONLY
    page_already_protected = p->first_tb != NULL;
#endif
    p->first_tb = (TranslationBlock *)((long)tb | n);
    invalidate_page_bitmap(p);

#if defined(TARGET_HAS_SMC) || 1

#if defined(CONFIG_USER_ONLY)
    if (p->flags & PAGE_WRITE) {
        target_ulong addr;
        PageDesc *p2;
        int prot;

        /* force the host page as non writable (writes will have a
           page fault + mprotect overhead) */
        page_addr &= qemu_host_page_mask;
        prot = 0;
        for(addr = page_addr; addr < page_addr + qemu_host_page_size;
            addr += TARGET_PAGE_SIZE) {

            p2 = page_find (addr >> TARGET_PAGE_BITS);
            if (!p2)
                continue;
            prot |= p2->flags;
            p2->flags &= ~PAGE_WRITE;
          }
        mprotect(g2h(page_addr), qemu_host_page_size,
                 (prot & PAGE_BITS) & ~PAGE_WRITE);
#ifdef DEBUG_TB_INVALIDATE
        printf("protecting code page: 0x" TARGET_FMT_lx "\n",
               page_addr);
#endif
    }
#else
    /* if some code is already present, then the pages are already
       protected. So we handle the case where only the first TB is
       allocated in a physical page */
    if (!page_already_protected) {
        tlb_protect_code(page_addr);
    }
#endif

#endif /* TARGET_HAS_SMC */
}

/* add a new TB and link it to the physical page tables. phys_page2 is
   (-1) to indicate that only one page contains the TB. */
void tb_link_page(TranslationBlock *tb,
                  tb_page_addr_t phys_pc, tb_page_addr_t phys_page2)
{
    unsigned int h;
    TranslationBlock **ptb;

    /* Grab the mmap lock to stop another thread invalidating this TB
       before we are done.  */
    mmap_lock();
    /* add in the physical hash table */
    h = tb_phys_hash_func(phys_pc);
    ptb = &tb_phys_hash[h];
    tb->phys_hash_next = *ptb;
    *ptb = tb;

    /* add in the page list */
    tb_alloc_page(tb, 0, phys_pc & TARGET_PAGE_MASK);
    if (phys_page2 != -1)
        tb_alloc_page(tb, 1, phys_page2);
    else
        tb->page_addr[1] = -1;

    tb->jmp_first = (TranslationBlock *)((long)tb | 2);
    tb->jmp_next[0] = NULL;
    tb->jmp_next[1] = NULL;

    /* init original jump addresses */
    if (tb->tb_next_offset[0] != 0xffff)
        tb_reset_jump(tb, 0);
    if (tb->tb_next_offset[1] != 0xffff)
        tb_reset_jump(tb, 1);

#ifdef DEBUG_TB_CHECK
    tb_page_check();
#endif
    mmap_unlock();
}

/* find the TB 'tb' such that tb[0].tc_ptr <= tc_ptr <
   tb[1].tc_ptr. Return NULL if not found */
TranslationBlock *tb_find_pc(unsigned long tc_ptr)
{
    int m_min, m_max, m;
    unsigned long v;
    TranslationBlock *tb;

    if (nb_tbs <= 0)
        return NULL;
    if (tc_ptr < (unsigned long)code_gen_buffer ||
        tc_ptr >= (unsigned long)code_gen_ptr)
        return NULL;
    /* binary search (cf Knuth) */
    m_min = 0;
    m_max = nb_tbs - 1;
    while (m_min <= m_max) {
        m = (m_min + m_max) >> 1;
        tb = &tbs[m];
        v = (unsigned long)tb->tc_ptr;
        if (v == tc_ptr)
            return tb;
        else if (tc_ptr < v) {
            m_max = m - 1;
        } else {
            m_min = m + 1;
        }
    }
    return &tbs[m_max];
}

static void tb_reset_jump_recursive(TranslationBlock *tb);

static inline void tb_reset_jump_recursive2(TranslationBlock *tb, int n)
{
    TranslationBlock *tb1, *tb_next, **ptb;
    unsigned int n1;

    tb1 = tb->jmp_next[n];
    if (tb1 != NULL) {
        /* find head of list */
        for(;;) {
            n1 = (long)tb1 & 3;
            tb1 = (TranslationBlock *)((long)tb1 & ~3);
            if (n1 == 2)
                break;
            tb1 = tb1->jmp_next[n1];
        }
        /* we are now sure now that tb jumps to tb1 */
        tb_next = tb1;

        /* remove tb from the jmp_first list */
        ptb = &tb_next->jmp_first;
        for(;;) {
            tb1 = *ptb;
            n1 = (long)tb1 & 3;
            tb1 = (TranslationBlock *)((long)tb1 & ~3);
            if (n1 == n && tb1 == tb)
                break;
            ptb = &tb1->jmp_next[n1];
        }
        *ptb = tb->jmp_next[n];
        tb->jmp_next[n] = NULL;

        /* suppress the jump to next tb in generated code */
        tb_reset_jump(tb, n);

        /* suppress jumps in the tb on which we could have jumped */
        tb_reset_jump_recursive(tb_next);
    }
}

static void tb_reset_jump_recursive(TranslationBlock *tb)
{
    tb_reset_jump_recursive2(tb, 0);
    tb_reset_jump_recursive2(tb, 1);
}

#if defined(TARGET_HAS_ICE)
#if defined(CONFIG_USER_ONLY)
static void breakpoint_invalidate(CPUState *env, target_ulong pc)
{
    tb_invalidate_phys_page_range(pc, pc + 1, 0);
}
#else
static void breakpoint_invalidate(CPUState *env, target_ulong pc)
{
    target_phys_addr_t addr;
    target_ulong pd;
    ram_addr_t ram_addr;
    PhysPageDesc *p;

    addr = cpu_get_phys_page_debug(env, pc);
    p = phys_page_find(addr >> TARGET_PAGE_BITS);
    if (!p) {
        pd = IO_MEM_UNASSIGNED;
    } else {
        pd = p->phys_offset;
    }
    ram_addr = (pd & TARGET_PAGE_MASK) | (pc & ~TARGET_PAGE_MASK);
    tb_invalidate_phys_page_range(ram_addr, ram_addr + 1, 0);
}
#endif
#endif /* TARGET_HAS_ICE */

#if defined(CONFIG_USER_ONLY)
void cpu_watchpoint_remove_all(CPUState *env, int mask)

{
}

int cpu_watchpoint_insert(CPUState *env, target_ulong addr, target_ulong len,
                          int flags, CPUWatchpoint **watchpoint)
{
    return -ENOSYS;
}
#else
/* Add a watchpoint.  */
int cpu_watchpoint_insert(CPUState *env, target_ulong addr, target_ulong len,
                          int flags, CPUWatchpoint **watchpoint)
{
    target_ulong len_mask = ~(len - 1);
    CPUWatchpoint *wp;

    /* sanity checks: allow power-of-2 lengths, deny unaligned watchpoints */
    if ((len != 1 && len != 2 && len != 4 && len != 8) || (addr & ~len_mask)) {
        fprintf(stderr, "qemu: tried to set invalid watchpoint at "
                TARGET_FMT_lx ", len=" TARGET_FMT_lu "\n", addr, len);
        return -EINVAL;
    }
    wp = g_malloc(sizeof(*wp));

    wp->vaddr = addr;
    wp->len_mask = len_mask;
    wp->flags = flags;

    /* keep all GDB-injected watchpoints in front */
    if (flags & BP_GDB)
        QTAILQ_INSERT_HEAD(&env->watchpoints, wp, entry);
    else
        QTAILQ_INSERT_TAIL(&env->watchpoints, wp, entry);

    tlb_flush_page(env, addr);

    if (watchpoint)
        *watchpoint = wp;
    return 0;
}

/* Remove a specific watchpoint.  */
int cpu_watchpoint_remove(CPUState *env, target_ulong addr, target_ulong len,
                          int flags)
{
    target_ulong len_mask = ~(len - 1);
    CPUWatchpoint *wp;

    QTAILQ_FOREACH(wp, &env->watchpoints, entry) {
        if (addr == wp->vaddr && len_mask == wp->len_mask
                && flags == (wp->flags & ~BP_WATCHPOINT_HIT)) {
            cpu_watchpoint_remove_by_ref(env, wp);
            return 0;
        }
    }
    return -ENOENT;
}

/* Remove a specific watchpoint by reference.  */
void cpu_watchpoint_remove_by_ref(CPUState *env, CPUWatchpoint *watchpoint)
{
    QTAILQ_REMOVE(&env->watchpoints, watchpoint, entry);

    tlb_flush_page(env, watchpoint->vaddr);

    g_free(watchpoint);
}

/* Remove all matching watchpoints.  */
void cpu_watchpoint_remove_all(CPUState *env, int mask)
{
    CPUWatchpoint *wp, *next;

    QTAILQ_FOREACH_SAFE(wp, &env->watchpoints, entry, next) {
        if (wp->flags & mask)
            cpu_watchpoint_remove_by_ref(env, wp);
    }
}
#endif

/* Add a breakpoint.  */
int cpu_breakpoint_insert(CPUState *env, target_ulong pc, int flags,
                          CPUBreakpoint **breakpoint)
{
#if defined(TARGET_HAS_ICE)
    CPUBreakpoint *bp;

    bp = g_malloc(sizeof(*bp));

    bp->pc = pc;
    bp->flags = flags;

    /* keep all GDB-injected breakpoints in front */
    if (flags & BP_GDB)
        QTAILQ_INSERT_HEAD(&env->breakpoints, bp, entry);
    else
        QTAILQ_INSERT_TAIL(&env->breakpoints, bp, entry);

    breakpoint_invalidate(env, pc);

    if (breakpoint)
        *breakpoint = bp;
    return 0;
#else
    return -ENOSYS;
#endif
}

/* Remove a specific breakpoint.  */
int cpu_breakpoint_remove(CPUState *env, target_ulong pc, int flags)
{
#if defined(TARGET_HAS_ICE)
    CPUBreakpoint *bp;

    QTAILQ_FOREACH(bp, &env->breakpoints, entry) {
        if (bp->pc == pc && bp->flags == flags) {
            cpu_breakpoint_remove_by_ref(env, bp);
            return 0;
        }
    }
    return -ENOENT;
#else
    return -ENOSYS;
#endif
}

/* Remove a specific breakpoint by reference.  */
void cpu_breakpoint_remove_by_ref(CPUState *env, CPUBreakpoint *breakpoint)
{
#if defined(TARGET_HAS_ICE)
    QTAILQ_REMOVE(&env->breakpoints, breakpoint, entry);

    breakpoint_invalidate(env, breakpoint->pc);

    g_free(breakpoint);
#endif
}

/* Remove all matching breakpoints. */
void cpu_breakpoint_remove_all(CPUState *env, int mask)
{
#if defined(TARGET_HAS_ICE)
    CPUBreakpoint *bp, *next;

    QTAILQ_FOREACH_SAFE(bp, &env->breakpoints, entry, next) {
        if (bp->flags & mask)
            cpu_breakpoint_remove_by_ref(env, bp);
    }
#endif
}

/* enable or disable single step mode. EXCP_DEBUG is returned by the
   CPU loop after each instruction */
void cpu_single_step(CPUState *env, int enabled)
{
#if defined(TARGET_HAS_ICE)
    if (env->singlestep_enabled != enabled) {
        env->singlestep_enabled = enabled;
        if (kvm_enabled())
            kvm_update_guest_debug(env, 0);
        else {
            /* must flush all the translated code to avoid inconsistencies */
            /* XXX: only flush what is necessary */
            tb_flush(env);
        }
    }
#endif
}

/* enable or disable low levels log */
void cpu_set_log(int log_flags)
{
    loglevel = log_flags;
    if (loglevel && !logfile) {
        logfile = fopen(logfilename, log_append ? "a" : "w");
        if (!logfile) {
            perror(logfilename);
            _exit(1);
        }
#if !defined(CONFIG_SOFTMMU)
        /* must avoid mmap() usage of glibc by setting a buffer "by hand" */
        {
            static char logfile_buf[4096];
            setvbuf(logfile, logfile_buf, _IOLBF, sizeof(logfile_buf));
        }
#elif !defined(_WIN32)
        /* Win32 doesn't support line-buffering and requires size >= 2 */
        setvbuf(logfile, NULL, _IOLBF, 0);
#endif
        log_append = 1;
    }
    if (!loglevel && logfile) {
        fclose(logfile);
        logfile = NULL;
    }
}

void cpu_set_log_filename(const char *filename)
{
    logfilename = strdup(filename);
    if (logfile) {
        fclose(logfile);
        logfile = NULL;
    }
    cpu_set_log(loglevel);
}

static void cpu_unlink_tb(CPUState *env)
{
    /* FIXME: TB unchaining isn't SMP safe.  For now just ignore the
       problem and hope the cpu will stop of its own accord.  For userspace
       emulation this often isn't actually as bad as it sounds.  Often
       signals are used primarily to interrupt blocking syscalls.  */
    TranslationBlock *tb;
    static spinlock_t interrupt_lock = SPIN_LOCK_UNLOCKED;

    spin_lock(&interrupt_lock);
    tb = env->current_tb;
    /* if the cpu is currently executing code, we must unlink it and
       all the potentially executing TB */
    if (tb) {
        env->current_tb = NULL;
        tb_reset_jump_recursive(tb);
    }
    spin_unlock(&interrupt_lock);
}

#ifndef CONFIG_USER_ONLY
/* mask must never be zero, except for A20 change call */
static void tcg_handle_interrupt(CPUState *env, int mask)
{
    int old_mask;

    old_mask = env->interrupt_request;
    env->interrupt_request |= mask;

    /*
     * If called from iothread context, wake the target cpu in
     * case its halted.
     */
    if (!qemu_cpu_is_self(env)) {
        qemu_cpu_kick(env);
        return;
    }

    if (use_icount) {
        env->icount_decr.u16.high = 0xffff;
        if (!can_do_io(env)
            && (mask & ~old_mask) != 0) {
            cpu_abort(env, "Raised interrupt while not in I/O function");
        }
    } else {
        cpu_unlink_tb(env);
    }
}

CPUInterruptHandler cpu_interrupt_handler = tcg_handle_interrupt;

#else /* CONFIG_USER_ONLY */

void cpu_interrupt(CPUState *env, int mask)
{
    env->interrupt_request |= mask;
    cpu_unlink_tb(env);
}
#endif /* CONFIG_USER_ONLY */

void cpu_reset_interrupt(CPUState *env, int mask)
{
    env->interrupt_request &= ~mask;
}

void cpu_exit(CPUState *env)
{
    env->exit_request = 1;
    cpu_unlink_tb(env);
}

const CPULogItem cpu_log_items[] = {
    { CPU_LOG_TB_OUT_ASM, "out_asm",
      "show generated host assembly code for each compiled TB" },
    { CPU_LOG_TB_IN_ASM, "in_asm",
      "show target assembly code for each compiled TB" },
    { CPU_LOG_TB_OP, "op",
      "show micro ops for each compiled TB" },
    { CPU_LOG_TB_OP_OPT, "op_opt",
      "show micro ops "
#ifdef TARGET_I386
      "before eflags optimization and "
#endif
      "after liveness analysis" },
    { CPU_LOG_INT, "int",
      "show interrupts/exceptions in short format" },
    { CPU_LOG_EXEC, "exec",
      "show trace before each executed TB (lots of logs)" },
    { CPU_LOG_TB_CPU, "cpu",
      "show CPU state before block translation" },
#ifdef TARGET_I386
    { CPU_LOG_PCALL, "pcall",
      "show protected mode far calls/returns/exceptions" },
    { CPU_LOG_RESET, "cpu_reset",
      "show CPU state before CPU resets" },
#endif
#ifdef DEBUG_IOPORT
    { CPU_LOG_IOPORT, "ioport",
      "show all i/o ports accesses" },
#endif
    { 0, NULL, NULL },
};

#ifndef CONFIG_USER_ONLY
static QLIST_HEAD(memory_client_list, CPUPhysMemoryClient) memory_client_list
    = QLIST_HEAD_INITIALIZER(memory_client_list);

static void cpu_notify_set_memory(target_phys_addr_t start_addr,
                                  ram_addr_t size,
                                  ram_addr_t phys_offset,
                                  bool log_dirty)
{
    CPUPhysMemoryClient *client;
    QLIST_FOREACH(client, &memory_client_list, list) {
        client->set_memory(client, start_addr, size, phys_offset, log_dirty);
    }
}

static int cpu_notify_sync_dirty_bitmap(target_phys_addr_t start,
                                        target_phys_addr_t end)
{
    CPUPhysMemoryClient *client;
    QLIST_FOREACH(client, &memory_client_list, list) {
        int r = client->sync_dirty_bitmap(client, start, end);
        if (r < 0)
            return r;
    }
    return 0;
}

static int cpu_notify_migration_log(int enable)
{
    CPUPhysMemoryClient *client;
    QLIST_FOREACH(client, &memory_client_list, list) {
        int r = client->migration_log(client, enable);
        if (r < 0)
            return r;
    }
    return 0;
}

struct last_map {
    target_phys_addr_t start_addr;
    ram_addr_t size;
    ram_addr_t phys_offset;
};

/* The l1_phys_map provides the upper P_L1_BITs of the guest physical
 * address.  Each intermediate table provides the next L2_BITs of guest
 * physical address space.  The number of levels vary based on host and
 * guest configuration, making it efficient to build the final guest
 * physical address by seeding the L1 offset and shifting and adding in
 * each L2 offset as we recurse through them. */
static void phys_page_for_each_1(CPUPhysMemoryClient *client, int level,
                                 void **lp, target_phys_addr_t addr,
                                 struct last_map *map)
{
    int i;

    if (*lp == NULL) {
        return;
    }
    if (level == 0) {
        PhysPageDesc *pd = *lp;
        addr <<= L2_BITS + TARGET_PAGE_BITS;
        for (i = 0; i < L2_SIZE; ++i) {
            if (pd[i].phys_offset != IO_MEM_UNASSIGNED) {
                target_phys_addr_t start_addr = addr | i << TARGET_PAGE_BITS;

                if (map->size &&
                    start_addr == map->start_addr + map->size &&
                    pd[i].phys_offset == map->phys_offset + map->size) {

                    map->size += TARGET_PAGE_SIZE;
                    continue;
                } else if (map->size) {
                    client->set_memory(client, map->start_addr,
                                       map->size, map->phys_offset, false);
                }

                map->start_addr = start_addr;
                map->size = TARGET_PAGE_SIZE;
                map->phys_offset = pd[i].phys_offset;
            }
        }
    } else {
        void **pp = *lp;
        for (i = 0; i < L2_SIZE; ++i) {
            phys_page_for_each_1(client, level - 1, pp + i,
                                 (addr << L2_BITS) | i, map);
        }
    }
}

static void phys_page_for_each(CPUPhysMemoryClient *client)
{
    int i;
    struct last_map map = { };

    for (i = 0; i < P_L1_SIZE; ++i) {
        phys_page_for_each_1(client, P_L1_SHIFT / L2_BITS - 1,
                             l1_phys_map + i, i, &map);
    }
    if (map.size) {
        client->set_memory(client, map.start_addr, map.size, map.phys_offset,
                           false);
    }
}

void cpu_register_phys_memory_client(CPUPhysMemoryClient *client)
{
    QLIST_INSERT_HEAD(&memory_client_list, client, list);
    phys_page_for_each(client);
}

void cpu_unregister_phys_memory_client(CPUPhysMemoryClient *client)
{
    QLIST_REMOVE(client, list);
}
#endif

static int cmp1(const char *s1, int n, const char *s2)
{
    if (strlen(s2) != n)
        return 0;
    return memcmp(s1, s2, n) == 0;
}

/* takes a comma separated list of log masks. Return 0 if error. */
int cpu_str_to_log_mask(const char *str)
{
    const CPULogItem *item;
    int mask;
    const char *p, *p1;

    p = str;
    mask = 0;
    for(;;) {
        p1 = strchr(p, ',');
        if (!p1)
            p1 = p + strlen(p);
        if(cmp1(p,p1-p,"all")) {
            for(item = cpu_log_items; item->mask != 0; item++) {
                mask |= item->mask;
            }
        } else {
            for(item = cpu_log_items; item->mask != 0; item++) {
                if (cmp1(p, p1 - p, item->name))
                    goto found;
            }
            return 0;
        }
    found:
        mask |= item->mask;
        if (*p1 != ',')
            break;
        p = p1 + 1;
    }
    return mask;
}

void cpu_abort(CPUState *env, const char *fmt, ...)
{
    va_list ap;
    va_list ap2;

    va_start(ap, fmt);
    va_copy(ap2, ap);
    fprintf(stderr, "qemu: fatal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
#ifdef TARGET_I386
    cpu_dump_state(env, stderr, fprintf, X86_DUMP_FPU | X86_DUMP_CCOP);
#else
    cpu_dump_state(env, stderr, fprintf, 0);
#endif
    if (qemu_log_enabled()) {
        qemu_log("qemu: fatal: ");
        qemu_log_vprintf(fmt, ap2);
        qemu_log("\n");
#ifdef TARGET_I386
        log_cpu_state(env, X86_DUMP_FPU | X86_DUMP_CCOP);
#else
        log_cpu_state(env, 0);
#endif
        qemu_log_flush();
        qemu_log_close();
    }
    va_end(ap2);
    va_end(ap);
#if defined(CONFIG_USER_ONLY)
    {
        struct sigaction act;
        sigfillset(&act.sa_mask);
        act.sa_handler = SIG_DFL;
        sigaction(SIGABRT, &act, NULL);
    }
#endif
    abort();
}

CPUState *cpu_copy(CPUState *env)
{
    CPUState *new_env = cpu_init(env->cpu_model_str);
    CPUState *next_cpu = new_env->next_cpu;
    int cpu_index = new_env->cpu_index;
#if defined(TARGET_HAS_ICE)
    CPUBreakpoint *bp;
    CPUWatchpoint *wp;
#endif

    memcpy(new_env, env, sizeof(CPUState));

    /* Preserve chaining and index. */
    new_env->next_cpu = next_cpu;
    new_env->cpu_index = cpu_index;

    /* Clone all break/watchpoints.
       Note: Once we support ptrace with hw-debug register access, make sure
       BP_CPU break/watchpoints are handled correctly on clone. */
    QTAILQ_INIT(&env->breakpoints);
    QTAILQ_INIT(&env->watchpoints);
#if defined(TARGET_HAS_ICE)
    QTAILQ_FOREACH(bp, &env->breakpoints, entry) {
        cpu_breakpoint_insert(new_env, bp->pc, bp->flags, NULL);
    }
    QTAILQ_FOREACH(wp, &env->watchpoints, entry) {
        cpu_watchpoint_insert(new_env, wp->vaddr, (~wp->len_mask) + 1,
                              wp->flags, NULL);
    }
#endif

    return new_env;
}

#if !defined(CONFIG_USER_ONLY)

static inline void tlb_flush_jmp_cache(CPUState *env, target_ulong addr)
{
    unsigned int i;

    /* Discard jump cache entries for any tb which might potentially
       overlap the flushed page.  */
    i = tb_jmp_cache_hash_page(addr - TARGET_PAGE_SIZE);
    memset (&env->tb_jmp_cache[i], 0, 
            TB_JMP_PAGE_SIZE * sizeof(TranslationBlock *));

    i = tb_jmp_cache_hash_page(addr);
    memset (&env->tb_jmp_cache[i], 0, 
            TB_JMP_PAGE_SIZE * sizeof(TranslationBlock *));
}

static CPUTLBEntry s_cputlb_empty_entry = {
    .addr_read  = -1,
    .addr_write = -1,
    .addr_code  = -1,
    .addend     = -1,
};

/* NOTE: if flush_global is true, also flush global entries (not
   implemented yet) */
void tlb_flush(CPUState *env, int flush_global)
{
    int i;

#if defined(DEBUG_TLB)
    printf("tlb_flush:\n");
#endif
    /* must reset current TB so that interrupts cannot modify the
       links while we are modifying them */
    env->current_tb = NULL;

    for(i = 0; i < CPU_TLB_SIZE; i++) {
        int mmu_idx;
        for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
            env->tlb_table[mmu_idx][i] = s_cputlb_empty_entry;
        }
    }

    memset (env->tb_jmp_cache, 0, TB_JMP_CACHE_SIZE * sizeof (void *));

    env->tlb_flush_addr = -1;
    env->tlb_flush_mask = 0;
    tlb_flush_count++;
}

static inline void tlb_flush_entry(CPUTLBEntry *tlb_entry, target_ulong addr)
{
    if (addr == (tlb_entry->addr_read &
                 (TARGET_PAGE_MASK | TLB_INVALID_MASK)) ||
        addr == (tlb_entry->addr_write &
                 (TARGET_PAGE_MASK | TLB_INVALID_MASK)) ||
        addr == (tlb_entry->addr_code &
                 (TARGET_PAGE_MASK | TLB_INVALID_MASK))) {
        *tlb_entry = s_cputlb_empty_entry;
    }
}

void tlb_flush_page(CPUState *env, target_ulong addr)
{
    int i;
    int mmu_idx;

#if defined(DEBUG_TLB)
    printf("tlb_flush_page: " TARGET_FMT_lx "\n", addr);
#endif
    /* Check if we need to flush due to large pages.  */
    if ((addr & env->tlb_flush_mask) == env->tlb_flush_addr) {
#if defined(DEBUG_TLB)
        printf("tlb_flush_page: forced full flush ("
               TARGET_FMT_lx "/" TARGET_FMT_lx ")\n",
               env->tlb_flush_addr, env->tlb_flush_mask);
#endif
        tlb_flush(env, 1);
        return;
    }
    /* must reset current TB so that interrupts cannot modify the
       links while we are modifying them */
    env->current_tb = NULL;

    addr &= TARGET_PAGE_MASK;
    i = (addr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);
    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++)
        tlb_flush_entry(&env->tlb_table[mmu_idx][i], addr);

    tlb_flush_jmp_cache(env, addr);
}

/* update the TLBs so that writes to code in the virtual page 'addr'
   can be detected */
static void tlb_protect_code(ram_addr_t ram_addr)
{
    cpu_physical_memory_reset_dirty(ram_addr,
                                    ram_addr + TARGET_PAGE_SIZE,
                                    CODE_DIRTY_FLAG);
}

/* update the TLB so that writes in physical page 'phys_addr' are no longer
   tested for self modifying code */
static void tlb_unprotect_code_phys(CPUState *env, ram_addr_t ram_addr,
                                    target_ulong vaddr)
{
    cpu_physical_memory_set_dirty_flags(ram_addr, CODE_DIRTY_FLAG);
}

static inline void tlb_reset_dirty_range(CPUTLBEntry *tlb_entry,
                                         unsigned long start, unsigned long length)
{
    unsigned long addr;
    if ((tlb_entry->addr_write & ~TARGET_PAGE_MASK) == IO_MEM_RAM) {
        addr = (tlb_entry->addr_write & TARGET_PAGE_MASK) + tlb_entry->addend;
        if ((addr - start) < length) {
            tlb_entry->addr_write = (tlb_entry->addr_write & TARGET_PAGE_MASK) | TLB_NOTDIRTY;
        }
    }
}

/* Note: start and end must be within the same ram block.  */
void cpu_physical_memory_reset_dirty(ram_addr_t start, ram_addr_t end,
                                     int dirty_flags)
{
    CPUState *env;
    unsigned long length, start1;
    int i;

    start &= TARGET_PAGE_MASK;
    end = TARGET_PAGE_ALIGN(end);

    length = end - start;
    if (length == 0)
        return;
    cpu_physical_memory_mask_dirty_range(start, length, dirty_flags);

    /* we modify the TLB cache so that the dirty bit will be set again
       when accessing the range */
    start1 = (unsigned long)qemu_safe_ram_ptr(start);
    /* Check that we don't span multiple blocks - this breaks the
       address comparisons below.  */
    if ((unsigned long)qemu_safe_ram_ptr(end - 1) - start1
            != (end - 1) - start) {
        abort();
    }

    for(env = first_cpu; env != NULL; env = env->next_cpu) {
        int mmu_idx;
        for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
            for(i = 0; i < CPU_TLB_SIZE; i++)
                tlb_reset_dirty_range(&env->tlb_table[mmu_idx][i],
                                      start1, length);
        }
    }
}

int cpu_physical_memory_set_dirty_tracking(int enable)
{
    int ret = 0;
    in_migration = enable;
    ret = cpu_notify_migration_log(!!enable);
    return ret;
}

int cpu_physical_memory_get_dirty_tracking(void)
{
    return in_migration;
}

int cpu_physical_sync_dirty_bitmap(target_phys_addr_t start_addr,
                                   target_phys_addr_t end_addr)
{
    int ret;
    ret = cpu_notify_sync_dirty_bitmap(start_addr, end_addr);
    return ret;
}

int cpu_physical_log_start(target_phys_addr_t start_addr,
                           ram_addr_t size)
{
    CPUPhysMemoryClient *client;
    QLIST_FOREACH(client, &memory_client_list, list) {
        if (client->log_start) {
            int r = client->log_start(client, start_addr, size);
            if (r < 0) {
                return r;
            }
        }
    }
    return 0;
}

int cpu_physical_log_stop(target_phys_addr_t start_addr,
                          ram_addr_t size)
{
    CPUPhysMemoryClient *client;
    QLIST_FOREACH(client, &memory_client_list, list) {
        if (client->log_stop) {
            int r = client->log_stop(client, start_addr, size);
            if (r < 0) {
                return r;
            }
        }
    }
    return 0;
}

static inline void tlb_update_dirty(CPUTLBEntry *tlb_entry)
{
    ram_addr_t ram_addr;
    void *p;

    if ((tlb_entry->addr_write & ~TARGET_PAGE_MASK) == IO_MEM_RAM) {
        p = (void *)(unsigned long)((tlb_entry->addr_write & TARGET_PAGE_MASK)
            + tlb_entry->addend);
        ram_addr = qemu_ram_addr_from_host_nofail(p);
        if (!cpu_physical_memory_is_dirty(ram_addr)) {
            tlb_entry->addr_write |= TLB_NOTDIRTY;
        }
    }
}

/* update the TLB according to the current state of the dirty bits */
void cpu_tlb_update_dirty(CPUState *env)
{
    int i;
    int mmu_idx;
    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        for(i = 0; i < CPU_TLB_SIZE; i++)
            tlb_update_dirty(&env->tlb_table[mmu_idx][i]);
    }
}

static inline void tlb_set_dirty1(CPUTLBEntry *tlb_entry, target_ulong vaddr)
{
    if (tlb_entry->addr_write == (vaddr | TLB_NOTDIRTY))
        tlb_entry->addr_write = vaddr;
}

/* update the TLB corresponding to virtual page vaddr
   so that it is no longer dirty */
static inline void tlb_set_dirty(CPUState *env, target_ulong vaddr)
{
    int i;
    int mmu_idx;

    vaddr &= TARGET_PAGE_MASK;
    i = (vaddr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);
    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++)
        tlb_set_dirty1(&env->tlb_table[mmu_idx][i], vaddr);
}

/* Our TLB does not support large pages, so remember the area covered by
   large pages and trigger a full TLB flush if these are invalidated.  */
static void tlb_add_large_page(CPUState *env, target_ulong vaddr,
                               target_ulong size)
{
    target_ulong mask = ~(size - 1);

    if (env->tlb_flush_addr == (target_ulong)-1) {
        env->tlb_flush_addr = vaddr & mask;
        env->tlb_flush_mask = mask;
        return;
    }
    /* Extend the existing region to include the new page.
       This is a compromise between unnecessary flushes and the cost
       of maintaining a full variable size TLB.  */
    mask &= env->tlb_flush_mask;
    while (((env->tlb_flush_addr ^ vaddr) & mask) != 0) {
        mask <<= 1;
    }
    env->tlb_flush_addr &= mask;
    env->tlb_flush_mask = mask;
}

/* Add a new TLB entry. At most one entry for a given virtual address
   is permitted. Only a single TARGET_PAGE_SIZE region is mapped, the
   supplied size is only used by tlb_flush_page.  */
void tlb_set_page(CPUState *env, target_ulong vaddr,
                  target_phys_addr_t paddr, int prot,
                  int mmu_idx, target_ulong size)
{
    PhysPageDesc *p;
    unsigned long pd;
    unsigned int index;
    target_ulong address;
    target_ulong code_address;
    unsigned long addend;
    CPUTLBEntry *te;
    CPUWatchpoint *wp;
    target_phys_addr_t iotlb;

    assert(size >= TARGET_PAGE_SIZE);
    if (size != TARGET_PAGE_SIZE) {
        tlb_add_large_page(env, vaddr, size);
    }
    p = phys_page_find(paddr >> TARGET_PAGE_BITS);
    if (!p) {
        pd = IO_MEM_UNASSIGNED;
    } else {
        pd = p->phys_offset;
    }
#if defined(DEBUG_TLB)
    printf("tlb_set_page: vaddr=" TARGET_FMT_lx " paddr=0x" TARGET_FMT_plx
           " prot=%x idx=%d pd=0x%08lx\n",
           vaddr, paddr, prot, mmu_idx, pd);
#endif

    address = vaddr;
    if ((pd & ~TARGET_PAGE_MASK) > IO_MEM_ROM && !(pd & IO_MEM_ROMD)) {
        /* IO memory case (romd handled later) */
        address |= TLB_MMIO;
    }
    addend = (unsigned long)qemu_get_ram_ptr(pd & TARGET_PAGE_MASK);
    if ((pd & ~TARGET_PAGE_MASK) <= IO_MEM_ROM) {
        /* Normal RAM.  */
        iotlb = pd & TARGET_PAGE_MASK;
        if ((pd & ~TARGET_PAGE_MASK) == IO_MEM_RAM)
            iotlb |= IO_MEM_NOTDIRTY;
        else
            iotlb |= IO_MEM_ROM;
    } else {
        /* IO handlers are currently passed a physical address.
           It would be nice to pass an offset from the base address
           of that region.  This would avoid having to special case RAM,
           and avoid full address decoding in every device.
           We can't use the high bits of pd for this because
           IO_MEM_ROMD uses these as a ram address.  */
        iotlb = (pd & ~TARGET_PAGE_MASK);
        if (p) {
            iotlb += p->region_offset;
        } else {
            iotlb += paddr;
        }
    }

    code_address = address;
    /* Make accesses to pages with watchpoints go via the
       watchpoint trap routines.  */
    QTAILQ_FOREACH(wp, &env->watchpoints, entry) {
        if (vaddr == (wp->vaddr & TARGET_PAGE_MASK)) {
            /* Avoid trapping reads of pages with a write breakpoint. */
            if ((prot & PAGE_WRITE) || (wp->flags & BP_MEM_READ)) {
                iotlb = io_mem_watch + paddr;
                address |= TLB_MMIO;
                break;
            }
        }
    }

    index = (vaddr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);
    env->iotlb[mmu_idx][index] = iotlb - vaddr;
    te = &env->tlb_table[mmu_idx][index];
    te->addend = addend - vaddr;
    if (prot & PAGE_READ) {
        te->addr_read = address;
    } else {
        te->addr_read = -1;
    }

    if (prot & PAGE_EXEC) {
        te->addr_code = code_address;
    } else {
        te->addr_code = -1;
    }
    if (prot & PAGE_WRITE) {
        if ((pd & ~TARGET_PAGE_MASK) == IO_MEM_ROM ||
            (pd & IO_MEM_ROMD)) {
            /* Write access calls the I/O callback.  */
            te->addr_write = address | TLB_MMIO;
        } else if ((pd & ~TARGET_PAGE_MASK) == IO_MEM_RAM &&
                   !cpu_physical_memory_is_dirty(pd)) {
            te->addr_write = address | TLB_NOTDIRTY;
        } else {
            te->addr_write = address;
        }
    } else {
        te->addr_write = -1;
    }
}

#else

void tlb_flush(CPUState *env, int flush_global)
{
}

void tlb_flush_page(CPUState *env, target_ulong addr)
{
}

/*
 * Walks guest process memory "regions" one by one
 * and calls callback function 'fn' for each region.
 */

struct walk_memory_regions_data
{
    walk_memory_regions_fn fn;
    void *priv;
    unsigned long start;
    int prot;
};

static int walk_memory_regions_end(struct walk_memory_regions_data *data,
                                   abi_ulong end, int new_prot)
{
    if (data->start != -1ul) {
        int rc = data->fn(data->priv, data->start, end, data->prot);
        if (rc != 0) {
            return rc;
        }
    }

    data->start = (new_prot ? end : -1ul);
    data->prot = new_prot;

    return 0;
}

static int walk_memory_regions_1(struct walk_memory_regions_data *data,
                                 abi_ulong base, int level, void **lp)
{
    abi_ulong pa;
    int i, rc;

    if (*lp == NULL) {
        return walk_memory_regions_end(data, base, 0);
    }

    if (level == 0) {
        PageDesc *pd = *lp;
        for (i = 0; i < L2_SIZE; ++i) {
            int prot = pd[i].flags;

            pa = base | (i << TARGET_PAGE_BITS);
            if (prot != data->prot) {
                rc = walk_memory_regions_end(data, pa, prot);
                if (rc != 0) {
                    return rc;
                }
            }
        }
    } else {
        void **pp = *lp;
        for (i = 0; i < L2_SIZE; ++i) {
            pa = base | ((abi_ulong)i <<
                (TARGET_PAGE_BITS + L2_BITS * level));
            rc = walk_memory_regions_1(data, pa, level - 1, pp + i);
            if (rc != 0) {
                return rc;
            }
        }
    }

    return 0;
}

int walk_memory_regions(void *priv, walk_memory_regions_fn fn)
{
    struct walk_memory_regions_data data;
    unsigned long i;

    data.fn = fn;
    data.priv = priv;
    data.start = -1ul;
    data.prot = 0;

    for (i = 0; i < V_L1_SIZE; i++) {
        int rc = walk_memory_regions_1(&data, (abi_ulong)i << V_L1_SHIFT,
                                       V_L1_SHIFT / L2_BITS - 1, l1_map + i);
        if (rc != 0) {
            return rc;
        }
    }

    return walk_memory_regions_end(&data, 0, 0);
}

static int dump_region(void *priv, abi_ulong start,
    abi_ulong end, unsigned long prot)
{
    FILE *f = (FILE *)priv;

    (void) fprintf(f, TARGET_ABI_FMT_lx"-"TARGET_ABI_FMT_lx
        " "TARGET_ABI_FMT_lx" %c%c%c\n",
        start, end, end - start,
        ((prot & PAGE_READ) ? 'r' : '-'),
        ((prot & PAGE_WRITE) ? 'w' : '-'),
        ((prot & PAGE_EXEC) ? 'x' : '-'));

    return (0);
}

/* dump memory mappings */
void page_dump(FILE *f)
{
    (void) fprintf(f, "%-8s %-8s %-8s %s\n",
            "start", "end", "size", "prot");
    walk_memory_regions(f, dump_region);
}

int page_get_flags(target_ulong address)
{
    PageDesc *p;

    p = page_find(address >> TARGET_PAGE_BITS);
    if (!p)
        return 0;
    return p->flags;
}

/* Modify the flags of a page and invalidate the code if necessary.
   The flag PAGE_WRITE_ORG is positioned automatically depending
   on PAGE_WRITE.  The mmap_lock should already be held.  */
void page_set_flags(target_ulong start, target_ulong end, int flags)
{
    target_ulong addr, len;

    /* This function should never be called with addresses outside the
       guest address space.  If this assert fires, it probably indicates
       a missing call to h2g_valid.  */
#if TARGET_ABI_BITS > L1_MAP_ADDR_SPACE_BITS
    assert(end < ((abi_ulong)1 << L1_MAP_ADDR_SPACE_BITS));
#endif
    assert(start < end);

    start = start & TARGET_PAGE_MASK;
    end = TARGET_PAGE_ALIGN(end);

    if (flags & PAGE_WRITE) {
        flags |= PAGE_WRITE_ORG;
    }

    for (addr = start, len = end - start;
         len != 0;
         len -= TARGET_PAGE_SIZE, addr += TARGET_PAGE_SIZE) {
        PageDesc *p = page_find_alloc(addr >> TARGET_PAGE_BITS, 1);

        /* If the write protection bit is set, then we invalidate
           the code inside.  */
        if (!(p->flags & PAGE_WRITE) &&
            (flags & PAGE_WRITE) &&
            p->first_tb) {
            tb_invalidate_phys_page(addr, 0, NULL);
        }
        p->flags = flags;
    }
}

int page_check_range(target_ulong start, target_ulong len, int flags)
{
    PageDesc *p;
    target_ulong end;
    target_ulong addr;

    /* This function should never be called with addresses outside the
       guest address space.  If this assert fires, it probably indicates
       a missing call to h2g_valid.  */
#if TARGET_ABI_BITS > L1_MAP_ADDR_SPACE_BITS
    assert(start < ((abi_ulong)1 << L1_MAP_ADDR_SPACE_BITS));
#endif

    if (len == 0) {
        return 0;
    }
    if (start + len - 1 < start) {
        /* We've wrapped around.  */
        return -1;
    }

    end = TARGET_PAGE_ALIGN(start+len); /* must do before we loose bits in the next step */
    start = start & TARGET_PAGE_MASK;

    for (addr = start, len = end - start;
         len != 0;
         len -= TARGET_PAGE_SIZE, addr += TARGET_PAGE_SIZE) {
        p = page_find(addr >> TARGET_PAGE_BITS);
        if( !p )
            return -1;
        if( !(p->flags & PAGE_VALID) )
            return -1;

        if ((flags & PAGE_READ) && !(p->flags & PAGE_READ))
            return -1;
        if (flags & PAGE_WRITE) {
            if (!(p->flags & PAGE_WRITE_ORG))
                return -1;
            /* unprotect the page if it was put read-only because it
               contains translated code */
            if (!(p->flags & PAGE_WRITE)) {
                if (!page_unprotect(addr, 0, NULL))
                    return -1;
            }
            return 0;
        }
    }
    return 0;
}

/* called from signal handler: invalidate the code and unprotect the
   page. Return TRUE if the fault was successfully handled. */
int page_unprotect(target_ulong address, unsigned long pc, void *puc)
{
    unsigned int prot;
    PageDesc *p;
    target_ulong host_start, host_end, addr;

    /* Technically this isn't safe inside a signal handler.  However we
       know this only ever happens in a synchronous SEGV handler, so in
       practice it seems to be ok.  */
    mmap_lock();

    p = page_find(address >> TARGET_PAGE_BITS);
    if (!p) {
        mmap_unlock();
        return 0;
    }

    /* if the page was really writable, then we change its
       protection back to writable */
    if ((p->flags & PAGE_WRITE_ORG) && !(p->flags & PAGE_WRITE)) {
        host_start = address & qemu_host_page_mask;
        host_end = host_start + qemu_host_page_size;

        prot = 0;
        for (addr = host_start ; addr < host_end ; addr += TARGET_PAGE_SIZE) {
            p = page_find(addr >> TARGET_PAGE_BITS);
            p->flags |= PAGE_WRITE;
            prot |= p->flags;

            /* and since the content will be modified, we must invalidate
               the corresponding translated code. */
            tb_invalidate_phys_page(addr, pc, puc);
#ifdef DEBUG_TB_CHECK
            tb_invalidate_check(addr);
#endif
        }
        mprotect((void *)g2h(host_start), qemu_host_page_size,
                 prot & PAGE_BITS);

        mmap_unlock();
        return 1;
    }
    mmap_unlock();
    return 0;
}

static inline void tlb_set_dirty(CPUState *env,
                                 unsigned long addr, target_ulong vaddr)
{
}
#endif /* defined(CONFIG_USER_ONLY) */

#if !defined(CONFIG_USER_ONLY)

#define SUBPAGE_IDX(addr) ((addr) & ~TARGET_PAGE_MASK)
typedef struct subpage_t {
    target_phys_addr_t base;
    ram_addr_t sub_io_index[TARGET_PAGE_SIZE];
    ram_addr_t region_offset[TARGET_PAGE_SIZE];
} subpage_t;

static int subpage_register (subpage_t *mmio, uint32_t start, uint32_t end,
                             ram_addr_t memory, ram_addr_t region_offset);
static subpage_t *subpage_init (target_phys_addr_t base, ram_addr_t *phys,
                                ram_addr_t orig_memory,
                                ram_addr_t region_offset);
#define CHECK_SUBPAGE(addr, start_addr, start_addr2, end_addr, end_addr2, \
                      need_subpage)                                     \
    do {                                                                \
        if (addr > start_addr)                                          \
            start_addr2 = 0;                                            \
        else {                                                          \
            start_addr2 = start_addr & ~TARGET_PAGE_MASK;               \
            if (start_addr2 > 0)                                        \
                need_subpage = 1;                                       \
        }                                                               \
                                                                        \
        if ((start_addr + orig_size) - addr >= TARGET_PAGE_SIZE)        \
            end_addr2 = TARGET_PAGE_SIZE - 1;                           \
        else {                                                          \
            end_addr2 = (start_addr + orig_size - 1) & ~TARGET_PAGE_MASK; \
            if (end_addr2 < TARGET_PAGE_SIZE - 1)                       \
                need_subpage = 1;                                       \
        }                                                               \
    } while (0)

/* register physical memory.
   For RAM, 'size' must be a multiple of the target page size.
   If (phys_offset & ~TARGET_PAGE_MASK) != 0, then it is an
   io memory page.  The address used when calling the IO function is
   the offset from the start of the region, plus region_offset.  Both
   start_addr and region_offset are rounded down to a page boundary
   before calculating this offset.  This should not be a problem unless
   the low bits of start_addr and region_offset differ.  */
void cpu_register_physical_memory_log(target_phys_addr_t start_addr,
                                         ram_addr_t size,
                                         ram_addr_t phys_offset,
                                         ram_addr_t region_offset,
                                         bool log_dirty)
{
    target_phys_addr_t addr, end_addr;
    PhysPageDesc *p;
    CPUState *env;
    ram_addr_t orig_size = size;
    subpage_t *subpage;

    assert(size);
    cpu_notify_set_memory(start_addr, size, phys_offset, log_dirty);

    if (phys_offset == IO_MEM_UNASSIGNED) {
        region_offset = start_addr;
    }
    region_offset &= TARGET_PAGE_MASK;
    size = (size + TARGET_PAGE_SIZE - 1) & TARGET_PAGE_MASK;
    end_addr = start_addr + (target_phys_addr_t)size;

    addr = start_addr;
    do {
        p = phys_page_find(addr >> TARGET_PAGE_BITS);
        if (p && p->phys_offset != IO_MEM_UNASSIGNED) {
            ram_addr_t orig_memory = p->phys_offset;
            target_phys_addr_t start_addr2, end_addr2;
            int need_subpage = 0;

            CHECK_SUBPAGE(addr, start_addr, start_addr2, end_addr, end_addr2,
                          need_subpage);
            if (need_subpage) {
                if (!(orig_memory & IO_MEM_SUBPAGE)) {
                    subpage = subpage_init((addr & TARGET_PAGE_MASK),
                                           &p->phys_offset, orig_memory,
                                           p->region_offset);
                } else {
                    subpage = io_mem_opaque[(orig_memory & ~TARGET_PAGE_MASK)
                                            >> IO_MEM_SHIFT];
                }
                subpage_register(subpage, start_addr2, end_addr2, phys_offset,
                                 region_offset);
                p->region_offset = 0;
            } else {
                p->phys_offset = phys_offset;
                if ((phys_offset & ~TARGET_PAGE_MASK) <= IO_MEM_ROM ||
                    (phys_offset & IO_MEM_ROMD))
                    phys_offset += TARGET_PAGE_SIZE;
            }
        } else {
            p = phys_page_find_alloc(addr >> TARGET_PAGE_BITS, 1);
            p->phys_offset = phys_offset;
            p->region_offset = region_offset;
            if ((phys_offset & ~TARGET_PAGE_MASK) <= IO_MEM_ROM ||
                (phys_offset & IO_MEM_ROMD)) {
                phys_offset += TARGET_PAGE_SIZE;
            } else {
                target_phys_addr_t start_addr2, end_addr2;
                int need_subpage = 0;

                CHECK_SUBPAGE(addr, start_addr, start_addr2, end_addr,
                              end_addr2, need_subpage);

                if (need_subpage) {
                    subpage = subpage_init((addr & TARGET_PAGE_MASK),
                                           &p->phys_offset, IO_MEM_UNASSIGNED,
                                           addr & TARGET_PAGE_MASK);
                    subpage_register(subpage, start_addr2, end_addr2,
                                     phys_offset, region_offset);
                    p->region_offset = 0;
                }
            }
        }
        region_offset += TARGET_PAGE_SIZE;
        addr += TARGET_PAGE_SIZE;
    } while (addr != end_addr);

    /* since each CPU stores ram addresses in its TLB cache, we must
       reset the modified entries */
    /* XXX: slow ! */
    for(env = first_cpu; env != NULL; env = env->next_cpu) {
        tlb_flush(env, 1);
    }
}

/* XXX: temporary until new memory mapping API */
ram_addr_t cpu_get_physical_page_desc(target_phys_addr_t addr)
{
    PhysPageDesc *p;

    p = phys_page_find(addr >> TARGET_PAGE_BITS);
    if (!p)
        return IO_MEM_UNASSIGNED;
    return p->phys_offset;
}

void qemu_register_coalesced_mmio(target_phys_addr_t addr, ram_addr_t size)
{
    if (kvm_enabled())
        kvm_coalesce_mmio_region(addr, size);
}

void qemu_unregister_coalesced_mmio(target_phys_addr_t addr, ram_addr_t size)
{
    if (kvm_enabled())
        kvm_uncoalesce_mmio_region(addr, size);
}

void qemu_flush_coalesced_mmio_buffer(void)
{
    if (kvm_enabled())
        kvm_flush_coalesced_mmio_buffer();
}

#if defined(__linux__) && !defined(TARGET_S390X)

#include <sys/vfs.h>

#define HUGETLBFS_MAGIC       0x958458f6

static long gethugepagesize(const char *path)
{
    struct statfs fs;
    int ret;

    do {
        ret = statfs(path, &fs);
    } while (ret != 0 && errno == EINTR);

    if (ret != 0) {
        perror(path);
        return 0;
    }

    if (fs.f_type != HUGETLBFS_MAGIC)
        fprintf(stderr, "Warning: path not on HugeTLBFS: %s\n", path);

    return fs.f_bsize;
}

static void *file_ram_alloc(RAMBlock *block,
                            ram_addr_t memory,
                            const char *path)
{
    char *filename;
    void *area;
    int fd;
#ifdef MAP_POPULATE
    int flags;
#endif
    unsigned long hpagesize;

    hpagesize = gethugepagesize(path);
    if (!hpagesize) {
        return NULL;
    }

    if (memory < hpagesize) {
        return NULL;
    }

    if (kvm_enabled() && !kvm_has_sync_mmu()) {
        fprintf(stderr, "host lacks kvm mmu notifiers, -mem-path unsupported\n");
        return NULL;
    }

    if (asprintf(&filename, "%s/qemu_back_mem.XXXXXX", path) == -1) {
        return NULL;
    }

    fd = mkstemp(filename);
    if (fd < 0) {
        perror("unable to create backing store for hugepages");
        free(filename);
        return NULL;
    }
    unlink(filename);
    free(filename);

    memory = (memory+hpagesize-1) & ~(hpagesize-1);

    /*
     * ftruncate is not supported by hugetlbfs in older
     * hosts, so don't bother bailing out on errors.
     * If anything goes wrong with it under other filesystems,
     * mmap will fail.
     */
    if (ftruncate(fd, memory))
        perror("ftruncate");

#ifdef MAP_POPULATE
    /* NB: MAP_POPULATE won't exhaustively alloc all phys pages in the case
     * MAP_PRIVATE is requested.  For mem_prealloc we mmap as MAP_SHARED
     * to sidestep this quirk.
     */
    flags = mem_prealloc ? MAP_POPULATE | MAP_SHARED : MAP_PRIVATE;
    area = mmap(0, memory, PROT_READ | PROT_WRITE, flags, fd, 0);
#else
    area = mmap(0, memory, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
#endif
    if (area == MAP_FAILED) {
        perror("file_ram_alloc: can't mmap RAM pages");
        close(fd);
        return (NULL);
    }
    block->fd = fd;
    return area;
}
#endif

static ram_addr_t find_ram_offset(ram_addr_t size)
{
    RAMBlock *block, *next_block;
    ram_addr_t offset = RAM_ADDR_MAX, mingap = RAM_ADDR_MAX;

    if (QLIST_EMPTY(&ram_list.blocks)){
	return 0;
    }

    QLIST_FOREACH(block, &ram_list.blocks, next) { 
        ram_addr_t end, next = RAM_ADDR_MAX;

        end = block->offset + block->length;

        QLIST_FOREACH(next_block, &ram_list.blocks, next) {
            if (next_block->offset >= end) {
                next = MIN(next, next_block->offset);
            }
        }
        if (next - end >= size && next - end < mingap) {
            offset = end;
            mingap = next - end;
        }
    }

    if (offset == RAM_ADDR_MAX) {
        fprintf(stderr, "Failed to find gap of requested size: %" PRIu64 "\n",
                (uint64_t)size);
        abort();
    }

    return offset;
}

static ram_addr_t last_ram_offset(void)
{
    RAMBlock *block;
    ram_addr_t last = 0;

    QLIST_FOREACH(block, &ram_list.blocks, next) 
        last = MAX(last, block->offset + block->length);

    return last;
}

uint64_t tlc_target_page_bits(void);

uint64_t tlc_target_page_bits(void){
    return TARGET_PAGE_BITS;
}

uint64_t tlc_target_page_size(void);

uint64_t tlc_target_page_size(void){
    return TARGET_PAGE_SIZE;
}

ram_addr_t tlc_last_ram_offset(void);

ram_addr_t tlc_last_ram_offset(void)
{
    RAMBlock *block;
    ram_addr_t last = 0;

    QLIST_FOREACH(block, &tlc_ram_list.blocks, next) // TLC ram
        last = MAX(last, block->offset + block->length);

    return last;
}

// TLC ram (we create contents of the tlc_ram_list.blocks variable here)

ram_addr_t qemu_ram_alloc_from_ptr(DeviceState *dev, const char *name,
                                   ram_addr_t size, void *host)
{
    RAMBlock *new_block, *block;

    RAMBlock *tlc_new_block, *tlc_block; // TLC ram
    
    size = TARGET_PAGE_ALIGN(size);
    new_block = g_malloc0(sizeof(*new_block));
    
    tlc_new_block = g_malloc0(sizeof(*tlc_new_block)); // TLC ram

    if (dev && dev->parent_bus && dev->parent_bus->info->get_dev_path) {
        char *id = dev->parent_bus->info->get_dev_path(dev);
        if (id) {
            snprintf(new_block->idstr, sizeof(new_block->idstr), "%s/", id);
	    snprintf(tlc_new_block->idstr, sizeof(tlc_new_block->idstr), "%s/", id); // TLC ram
            g_free(id);
        }
    }
    pstrcat(new_block->idstr, sizeof(new_block->idstr), name);
    pstrcat(tlc_new_block->idstr, sizeof(tlc_new_block->idstr), name); // TLC ram

    QLIST_FOREACH(block, &ram_list.blocks, next) { 
        if (!strcmp(block->idstr, new_block->idstr)) {
            fprintf(stderr, "RAMBlock \"%s\" already registered, abort!\n",
                    new_block->idstr);
            abort();
        }
    }

    // TLC ram begin
    QLIST_FOREACH(tlc_block, &tlc_ram_list.blocks, next) { // TLC ram
        if (!strcmp(tlc_block->idstr, tlc_new_block->idstr)) { // TLC ram
            fprintf(stderr, "RAMBlock \"%s\" already registered, abort!\n",
                    tlc_new_block->idstr); // TLC ram
            abort(); // TLC ram
        }
    } 
    // TLC ram end

    new_block->offset = find_ram_offset(size);
    tlc_new_block->offset = new_block->offset; // TLC ram
    if (host) {
        new_block->host = host;
	tlc_new_block->host = host; // TLC ram
        new_block->flags |= RAM_PREALLOC_MASK;
	tlc_new_block->flags |= RAM_PREALLOC_MASK; // TLC ram
    } else {
        if (mem_path) {
#if defined (__linux__) && !defined(TARGET_S390X)
            new_block->host = file_ram_alloc(new_block, size, mem_path);
	    tlc_new_block->host = new_block->host; // TLC ram
            if (!new_block->host) {
                new_block->host = qemu_vmalloc(size);
		tlc_new_block->host = new_block->host; // TLC ram
                qemu_madvise(new_block->host, size, QEMU_MADV_MERGEABLE);
            }
#else
            fprintf(stderr, "-mem-path option unsupported\n");
            exit(1);
#endif
        } else {
#if defined(TARGET_S390X) && defined(CONFIG_KVM)
            /* S390 KVM requires the topmost vma of the RAM to be smaller than
               an system defined value, which is at least 256GB. Larger systems
               have larger values. We put the guest between the end of data
               segment (system break) and this value. We use 32GB as a base to
               have enough room for the system break to grow. */
            new_block->host = mmap((void*)0x800000000, size,
                                   PROT_EXEC|PROT_READ|PROT_WRITE,
                                   MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            if (new_block->host == MAP_FAILED) {
                fprintf(stderr, "Allocating RAM failed\n");
                abort();
            }
	    tlc_new_block->host = new_block->host; // TLC ram
#else
            if (xen_enabled()) {
                xen_ram_alloc(new_block->offset, size);
            } else {
                new_block->host = qemu_vmalloc(size);
		tlc_new_block->host = new_block->host; // TLC ram
            }
#endif
            qemu_madvise(new_block->host, size, QEMU_MADV_MERGEABLE);
        }
    }
    new_block->length = size;
    tlc_new_block->length = new_block->length; // TLC ram

    QLIST_INSERT_HEAD(&ram_list.blocks, new_block, next); 
    
    QLIST_INSERT_HEAD(&tlc_ram_list.blocks, tlc_new_block, next); // TLC ram

    ram_list.phys_dirty = g_realloc(ram_list.phys_dirty,
                                       last_ram_offset() >> TARGET_PAGE_BITS);
    memset(ram_list.phys_dirty + (new_block->offset >> TARGET_PAGE_BITS),
           0xff, size >> TARGET_PAGE_BITS);
    
    // TLC ram begin
    //     Duplicate the dirtybit array for TLC.
    tlc_ram_list.phys_dirty = g_realloc(tlc_ram_list.phys_dirty,
                                       tlc_last_ram_offset() >> TARGET_PAGE_BITS);
    memset(tlc_ram_list.phys_dirty + (tlc_new_block->offset >> TARGET_PAGE_BITS),
           0xff, size >> TARGET_PAGE_BITS);    
    // TLC ram end

    if (kvm_enabled())
        kvm_setup_guest_memory(new_block->host, size);

    return new_block->offset;
}

ram_addr_t qemu_ram_alloc(DeviceState *dev, const char *name, ram_addr_t size)
{
    return qemu_ram_alloc_from_ptr(dev, name, size, NULL);
}

void qemu_ram_free_from_ptr(ram_addr_t addr)
{
    RAMBlock *block;

    QLIST_FOREACH(block, &ram_list.blocks, next) { 
        if (addr == block->offset) {
            QLIST_REMOVE(block, next);
            g_free(block);
            return;
        }
    }
}

void qemu_ram_free(ram_addr_t addr)
{
    RAMBlock *block;

    QLIST_FOREACH(block, &ram_list.blocks, next) { 
        if (addr == block->offset) {
            QLIST_REMOVE(block, next);
            if (block->flags & RAM_PREALLOC_MASK) {
                ;
            } else if (mem_path) {
#if defined (__linux__) && !defined(TARGET_S390X)
                if (block->fd) {
                    munmap(block->host, block->length);
                    close(block->fd);
                } else {
                    qemu_vfree(block->host);
                }
#else
		abort();
#endif
            } else {
#if defined(TARGET_S390X) && defined(CONFIG_KVM)
                munmap(block->host, block->length);
#else
                if (xen_enabled()) {
                    xen_invalidate_map_cache_entry(block->host);
                } else {
                    qemu_vfree(block->host);
                }
#endif
            }
            g_free(block);
            return;
        }
    }

}

#ifndef _WIN32
void qemu_ram_remap(ram_addr_t addr, ram_addr_t length)
{
    RAMBlock *block;
    ram_addr_t offset;
    int flags;
    void *area, *vaddr;

    QLIST_FOREACH(block, &ram_list.blocks, next) { 
        offset = addr - block->offset;
        if (offset < block->length) {
            vaddr = block->host + offset;
            if (block->flags & RAM_PREALLOC_MASK) {
                ;
            } else {
                flags = MAP_FIXED;
                munmap(vaddr, length);
                if (mem_path) {
#if defined(__linux__) && !defined(TARGET_S390X)
                    if (block->fd) {
#ifdef MAP_POPULATE
                        flags |= mem_prealloc ? MAP_POPULATE | MAP_SHARED :
                            MAP_PRIVATE;
#else
                        flags |= MAP_PRIVATE;
#endif
                        area = mmap(vaddr, length, PROT_READ | PROT_WRITE,
                                    flags, block->fd, offset);
                    } else {
                        flags |= MAP_PRIVATE | MAP_ANONYMOUS;
                        area = mmap(vaddr, length, PROT_READ | PROT_WRITE,
                                    flags, -1, 0);
                    }
#else
		    abort();
#endif
                } else {
#if defined(TARGET_S390X) && defined(CONFIG_KVM)
                    flags |= MAP_SHARED | MAP_ANONYMOUS;
                    area = mmap(vaddr, length, PROT_EXEC|PROT_READ|PROT_WRITE,
                                flags, -1, 0);
#else
                    flags |= MAP_PRIVATE | MAP_ANONYMOUS;
                    area = mmap(vaddr, length, PROT_READ | PROT_WRITE,
                                flags, -1, 0);
#endif
                }
                if (area != vaddr) {
                    fprintf(stderr, "Could not remap addr: "
                            RAM_ADDR_FMT "@" RAM_ADDR_FMT "\n",
                            length, addr);
                    exit(1);
                }
                qemu_madvise(vaddr, length, QEMU_MADV_MERGEABLE);
            }
            return;
        }
    }
}
#endif /* !_WIN32 */

/* Return a host pointer to ram allocated with qemu_ram_alloc.
   With the exception of the softmmu code in this file, this should
   only be used for local memory (e.g. video ram) that the device owns,
   and knows it isn't going to access beyond the end of the block.

   It should not be used for general purpose DMA.
   Use cpu_physical_memory_map/cpu_physical_memory_rw instead.
 */
// TLC supported get ram pointer function
void *tlc_qemu_get_ram_ptr(ram_addr_t addr)
{
    RAMBlock *tlc_block;

    pthread_mutex_lock(&acquire_tlc_ram_blocks); // TLC
    QLIST_FOREACH(tlc_block, &tlc_ram_list.blocks, next) { // TLC modified
        if ((addr - tlc_block->offset < tlc_block->length)
		&&(addr >= tlc_block->offset)) { // TLC: to guarantee the right block
            /* Move this entry to to start of the list.  */
            if (tlc_block != QLIST_FIRST(&tlc_ram_list.blocks)) {
                QLIST_REMOVE(tlc_block, next);
                QLIST_INSERT_HEAD(&tlc_ram_list.blocks, tlc_block, next);
            }
            if (xen_enabled()) {
                /* We need to check if the requested address is in the RAM
                 * because we don't want to map the entire memory in QEMU.
                 * In that case just map until the end of the page.
                 */
                if (tlc_block->offset == 0) {
		    pthread_mutex_unlock(&acquire_tlc_ram_blocks); // TLC
                    return xen_map_cache(addr, 0, 0);
                } else if (tlc_block->host == NULL) {
                    tlc_block->host =
                        xen_map_cache(tlc_block->offset, tlc_block->length, 1);
                }
            }
	    pthread_mutex_unlock(&acquire_tlc_ram_blocks); // TLC
            return tlc_block->host + (addr - tlc_block->offset);
        }
    }
    pthread_mutex_unlock(&acquire_tlc_ram_blocks); // TLC

    fprintf(stderr, "Bad ram offset %" PRIx64 "\n", (uint64_t)addr);
    abort();
    return NULL;
}

void tlc_display_ram_list(void);

// TLC ram begin
void tlc_display_ram_list(void){
    RAMBlock *block, *tlc_block;
    int i = 0;
    
    QLIST_FOREACH(block, &ram_list.blocks, next) {
        printf(" block [%d]: name = %s\n", i, block->idstr);
	printf("   offset=%" PRId64 ", len=%" PRId64 "\n", 
	    (uint64_t)block->offset, (uint64_t)block->length);
	printf("   flag=%d, host=%" PRIx64 " \n", 
	    (uint64_t)block->flags, (uint64_t)block->host);
	i++;
    }
    
    i = 0;
    QLIST_FOREACH(tlc_block, &tlc_ram_list.blocks, next) {
        printf(" tlc_block [%d]: name = %s\n", i, tlc_block->idstr);
	printf("   offset=%" PRId64 ", len=%" PRId64 "\n", 
	    (uint64_t)tlc_block->offset, (uint64_t)tlc_block->length);
	printf("   flag=%d, host=%" PRIx64 "\n", 
	    (uint64_t)tlc_block->flags, (uint64_t)tlc_block->host);
	i++;
    }
}
// TLC ram end

void *qemu_get_ram_ptr(ram_addr_t addr)
{
    RAMBlock *block;

    QLIST_FOREACH(block, &ram_list.blocks, next) { 
        if (addr - block->offset < block->length) {
            /* Move this entry to to start of the list.  */
            if (block != QLIST_FIRST(&ram_list.blocks)) {
                QLIST_REMOVE(block, next);
                QLIST_INSERT_HEAD(&ram_list.blocks, block, next);
            }
            if (xen_enabled()) {
                /* We need to check if the requested address is in the RAM
                 * because we don't want to map the entire memory in QEMU.
                 * In that case just map until the end of the page.
                 */
                if (block->offset == 0) {
                    return xen_map_cache(addr, 0, 0);
                } else if (block->host == NULL) {
                    block->host =
                        xen_map_cache(block->offset, block->length, 1);
                }
            }
            return block->host + (addr - block->offset);
        }
    }

    fprintf(stderr, "Bad ram offset %" PRIx64 "\n", (uint64_t)addr);
    abort();

    return NULL;
}

/* Return a host pointer to ram allocated with qemu_ram_alloc.
 * Same as qemu_get_ram_ptr but avoid reordering ramblocks.
 */
void *qemu_safe_ram_ptr(ram_addr_t addr)
{
    RAMBlock *block;

    QLIST_FOREACH(block, &ram_list.blocks, next) { 
        if (addr - block->offset < block->length) {
            if (xen_enabled()) {
                /* We need to check if the requested address is in the RAM
                 * because we don't want to map the entire memory in QEMU.
                 * In that case just map until the end of the page.
                 */
                if (block->offset == 0) {
                    return xen_map_cache(addr, 0, 0);
                } else if (block->host == NULL) {
                    block->host =
                        xen_map_cache(block->offset, block->length, 1);
                }
            }
            return block->host + (addr - block->offset);
        }
    }

    fprintf(stderr, "Bad ram offset %" PRIx64 "\n", (uint64_t)addr);
    abort();

    return NULL;
}

/* Return a host pointer to guest's ram. Similar to qemu_get_ram_ptr
 * but takes a size argument */
void *qemu_ram_ptr_length(ram_addr_t addr, ram_addr_t *size)
{
    if (*size == 0) {
        return NULL;
    }
    if (xen_enabled()) {
        return xen_map_cache(addr, *size, 1);
    } else {
        RAMBlock *block;

	QLIST_FOREACH(block, &ram_list.blocks, next) { 
            if (addr - block->offset < block->length) {
                if (addr - block->offset + *size > block->length){
                    *size = block->length - addr + block->offset;
		}
                return block->host + (addr - block->offset);
            }
        }

        fprintf(stderr, "Bad ram offset %" PRIx64 "\n", (uint64_t)addr);
        abort();
    }
}

void qemu_put_ram_ptr(void *addr)
{
    trace_qemu_put_ram_ptr(addr);
}

int qemu_ram_addr_from_host(void *ptr, ram_addr_t *ram_addr)
{
    RAMBlock *block;
    uint8_t *host = ptr;

    if (xen_enabled()) {
        *ram_addr = xen_ram_addr_from_mapcache(ptr);
        return 0;
    }

    QLIST_FOREACH(block, &ram_list.blocks, next) { 
        /* This case append when the block is not mapped. */
        if (block->host == NULL) {
            continue;
        }
        if (host - block->host < block->length) {
            *ram_addr = block->offset + (host - block->host);
            return 0;
        }
    }

    return -1;
}

/* Some of the softmmu routines need to translate from a host pointer
   (typically a TLB entry) back to a ram offset.  */
ram_addr_t qemu_ram_addr_from_host_nofail(void *ptr)
{
    ram_addr_t ram_addr;

    if (qemu_ram_addr_from_host(ptr, &ram_addr)) {
        fprintf(stderr, "Bad ram pointer %p\n", ptr);
        abort();
    }
    return ram_addr;
}

static uint32_t unassigned_mem_readb(void *opaque, target_phys_addr_t addr)
{
#ifdef DEBUG_UNASSIGNED
    printf("Unassigned mem read " TARGET_FMT_plx "\n", addr);
#endif
#if defined(TARGET_ALPHA) || defined(TARGET_SPARC) || defined(TARGET_MICROBLAZE)
    cpu_unassigned_access(cpu_single_env, addr, 0, 0, 0, 1);
#endif
    return 0;
}

static uint32_t unassigned_mem_readw(void *opaque, target_phys_addr_t addr)
{
#ifdef DEBUG_UNASSIGNED
    printf("Unassigned mem read " TARGET_FMT_plx "\n", addr);
#endif
#if defined(TARGET_ALPHA) || defined(TARGET_SPARC) || defined(TARGET_MICROBLAZE)
    cpu_unassigned_access(cpu_single_env, addr, 0, 0, 0, 2);
#endif
    return 0;
}

static uint32_t unassigned_mem_readl(void *opaque, target_phys_addr_t addr)
{
#ifdef DEBUG_UNASSIGNED
    printf("Unassigned mem read " TARGET_FMT_plx "\n", addr);
#endif
#if defined(TARGET_ALPHA) || defined(TARGET_SPARC) || defined(TARGET_MICROBLAZE)
    cpu_unassigned_access(cpu_single_env, addr, 0, 0, 0, 4);
#endif
    return 0;
}

static void unassigned_mem_writeb(void *opaque, target_phys_addr_t addr, uint32_t val)
{
#ifdef DEBUG_UNASSIGNED
    printf("Unassigned mem write " TARGET_FMT_plx " = 0x%x\n", addr, val);
#endif
#if defined(TARGET_ALPHA) || defined(TARGET_SPARC) || defined(TARGET_MICROBLAZE)
    cpu_unassigned_access(cpu_single_env, addr, 1, 0, 0, 1);
#endif
}

static void unassigned_mem_writew(void *opaque, target_phys_addr_t addr, uint32_t val)
{
#ifdef DEBUG_UNASSIGNED
    printf("Unassigned mem write " TARGET_FMT_plx " = 0x%x\n", addr, val);
#endif
#if defined(TARGET_ALPHA) || defined(TARGET_SPARC) || defined(TARGET_MICROBLAZE)
    cpu_unassigned_access(cpu_single_env, addr, 1, 0, 0, 2);
#endif
}

static void unassigned_mem_writel(void *opaque, target_phys_addr_t addr, uint32_t val)
{
#ifdef DEBUG_UNASSIGNED
    printf("Unassigned mem write " TARGET_FMT_plx " = 0x%x\n", addr, val);
#endif
#if defined(TARGET_ALPHA) || defined(TARGET_SPARC) || defined(TARGET_MICROBLAZE)
    cpu_unassigned_access(cpu_single_env, addr, 1, 0, 0, 4);
#endif
}

static CPUReadMemoryFunc * const unassigned_mem_read[3] = {
    unassigned_mem_readb,
    unassigned_mem_readw,
    unassigned_mem_readl,
};

static CPUWriteMemoryFunc * const unassigned_mem_write[3] = {
    unassigned_mem_writeb,
    unassigned_mem_writew,
    unassigned_mem_writel,
};

static void notdirty_mem_writeb(void *opaque, target_phys_addr_t ram_addr,
                                uint32_t val)
{
    if(unlikely(mthread)){
    
    int dirty_flags;
    dirty_flags = cpu_physical_memory_get_dirty_flags(ram_addr);
    if (!(dirty_flags & CODE_DIRTY_FLAG)) {
#if !defined(CONFIG_USER_ONLY)
        tb_invalidate_phys_page_fast(ram_addr, 1);
        dirty_flags = cpu_physical_memory_get_dirty_flags(ram_addr);
#endif
    }
    stb_p(qemu_get_ram_ptr(ram_addr), val);
    dirty_flags |= (0xff & ~CODE_DIRTY_FLAG & ~IO_DIRTY_FLAG);
    cpu_physical_memory_set_dirty_flags(ram_addr, dirty_flags);
    /* we remove the notdirty callback only if the code has been
       flushed */
    if ((dirty_flags == 0xff) || (dirty_flags == (~IO_DIRTY_FLAG))){
        tlb_set_dirty(cpu_single_env, cpu_single_env->mem_io_vaddr);
    }
    
    } 
    else{ // !mthread
    
    int dirty_flags;
    dirty_flags = cpu_physical_memory_get_dirty_flags(ram_addr);
    if (!(dirty_flags & CODE_DIRTY_FLAG)) {
#if !defined(CONFIG_USER_ONLY)
        tb_invalidate_phys_page_fast(ram_addr, 1);
        dirty_flags = cpu_physical_memory_get_dirty_flags(ram_addr);
#endif
    }
    stb_p(qemu_get_ram_ptr(ram_addr), val);
    dirty_flags |= (0xff & ~CODE_DIRTY_FLAG);
    cpu_physical_memory_set_dirty_flags(ram_addr, dirty_flags);
    /* we remove the notdirty callback only if the code has been
       flushed */
    if ((dirty_flags == 0xff) || (dirty_flags == (~IO_DIRTY_FLAG))){
        tlb_set_dirty(cpu_single_env, cpu_single_env->mem_io_vaddr);
    }
    
    }
}

static void notdirty_mem_writew(void *opaque, target_phys_addr_t ram_addr,
                                uint32_t val)
{
    if(unlikely(mthread)){
    
    int dirty_flags;
    dirty_flags = cpu_physical_memory_get_dirty_flags(ram_addr);
    if (!(dirty_flags & CODE_DIRTY_FLAG)) {
#if !defined(CONFIG_USER_ONLY)
        tb_invalidate_phys_page_fast(ram_addr, 2);
        dirty_flags = cpu_physical_memory_get_dirty_flags(ram_addr);
#endif
    }
    stw_p(qemu_get_ram_ptr(ram_addr), val);
    dirty_flags |= (0xff & ~CODE_DIRTY_FLAG & ~IO_DIRTY_FLAG);
    cpu_physical_memory_set_dirty_flags(ram_addr, dirty_flags);
    /* we remove the notdirty callback only if the code has been
       flushed */
    if ((dirty_flags == 0xff) || (dirty_flags == (~IO_DIRTY_FLAG))){
        tlb_set_dirty(cpu_single_env, cpu_single_env->mem_io_vaddr);
    }
    
    }
    else{ // !mthread
    
    int dirty_flags;
    dirty_flags = cpu_physical_memory_get_dirty_flags(ram_addr);
    if (!(dirty_flags & CODE_DIRTY_FLAG)) {
#if !defined(CONFIG_USER_ONLY)
        tb_invalidate_phys_page_fast(ram_addr, 2);
        dirty_flags = cpu_physical_memory_get_dirty_flags(ram_addr);
#endif
    }
    stw_p(qemu_get_ram_ptr(ram_addr), val);
    dirty_flags |= (0xff & ~CODE_DIRTY_FLAG);
    cpu_physical_memory_set_dirty_flags(ram_addr, dirty_flags);
    /* we remove the notdirty callback only if the code has been
       flushed */
    if ((dirty_flags == 0xff) || (dirty_flags == (~IO_DIRTY_FLAG))){
        tlb_set_dirty(cpu_single_env, cpu_single_env->mem_io_vaddr);
    }
    
    }
}

static void notdirty_mem_writel(void *opaque, target_phys_addr_t ram_addr,
                                uint32_t val)
{
    if(unlikely(mthread)){
    
    int dirty_flags;
    dirty_flags = cpu_physical_memory_get_dirty_flags(ram_addr);
    if (!(dirty_flags & CODE_DIRTY_FLAG)) {
#if !defined(CONFIG_USER_ONLY)
        tb_invalidate_phys_page_fast(ram_addr, 4);
        dirty_flags = cpu_physical_memory_get_dirty_flags(ram_addr);
#endif
    }
    stl_p(qemu_get_ram_ptr(ram_addr), val);
    dirty_flags |= (0xff & ~CODE_DIRTY_FLAG & ~IO_DIRTY_FLAG);
    cpu_physical_memory_set_dirty_flags(ram_addr, dirty_flags);
    /* we remove the notdirty callback only if the code has been
       flushed */
    if ((dirty_flags == 0xff) || (dirty_flags == (~IO_DIRTY_FLAG))){
        tlb_set_dirty(cpu_single_env, cpu_single_env->mem_io_vaddr);
    }
    
    }
    else{ // !mthread
    
    int dirty_flags;
    dirty_flags = cpu_physical_memory_get_dirty_flags(ram_addr);
    if (!(dirty_flags & CODE_DIRTY_FLAG)) {
#if !defined(CONFIG_USER_ONLY)
        tb_invalidate_phys_page_fast(ram_addr, 4);
        dirty_flags = cpu_physical_memory_get_dirty_flags(ram_addr);
#endif
    }
    stl_p(qemu_get_ram_ptr(ram_addr), val);
    dirty_flags |= (0xff & ~CODE_DIRTY_FLAG);
    cpu_physical_memory_set_dirty_flags(ram_addr, dirty_flags);
    /* we remove the notdirty callback only if the code has been
       flushed */
    if ((dirty_flags == 0xff) || (dirty_flags == (~IO_DIRTY_FLAG))){
        tlb_set_dirty(cpu_single_env, cpu_single_env->mem_io_vaddr);
    }
    
    }
}

static CPUReadMemoryFunc * const error_mem_read[3] = {
    NULL, /* never used */
    NULL, /* never used */
    NULL, /* never used */
};

static CPUWriteMemoryFunc * const notdirty_mem_write[3] = {
    notdirty_mem_writeb,
    notdirty_mem_writew,
    notdirty_mem_writel,
};

/* Generate a debug exception if a watchpoint has been hit.  */
static void check_watchpoint(int offset, int len_mask, int flags)
{
    CPUState *env = cpu_single_env;
    target_ulong pc, cs_base;
    TranslationBlock *tb;
    target_ulong vaddr;
    CPUWatchpoint *wp;
    int cpu_flags;

    if (env->watchpoint_hit) {
        /* We re-entered the check after replacing the TB. Now raise
         * the debug interrupt so that is will trigger after the
         * current instruction. */
        cpu_interrupt(env, CPU_INTERRUPT_DEBUG);
        return;
    }
    vaddr = (env->mem_io_vaddr & TARGET_PAGE_MASK) + offset;
    QTAILQ_FOREACH(wp, &env->watchpoints, entry) {
        if ((vaddr == (wp->vaddr & len_mask) ||
             (vaddr & wp->len_mask) == wp->vaddr) && (wp->flags & flags)) {
            wp->flags |= BP_WATCHPOINT_HIT;
            if (!env->watchpoint_hit) {
                env->watchpoint_hit = wp;
                tb = tb_find_pc(env->mem_io_pc);
                if (!tb) {
                    cpu_abort(env, "check_watchpoint: could not find TB for "
                              "pc=%p", (void *)env->mem_io_pc);
                }
                cpu_restore_state(tb, env, env->mem_io_pc);
                tb_phys_invalidate(tb, -1);
                if (wp->flags & BP_STOP_BEFORE_ACCESS) {
                    env->exception_index = EXCP_DEBUG;
                } else {
                    cpu_get_tb_cpu_state(env, &pc, &cs_base, &cpu_flags);
                    tb_gen_code(env, pc, cs_base, cpu_flags, 1);
                }
                cpu_resume_from_signal(env, NULL);
            }
        } else {
            wp->flags &= ~BP_WATCHPOINT_HIT;
        }
    }
}

/* Watchpoint access routines.  Watchpoints are inserted using TLB tricks,
   so these check for a hit then pass through to the normal out-of-line
   phys routines.  */
static uint32_t watch_mem_readb(void *opaque, target_phys_addr_t addr)
{
    check_watchpoint(addr & ~TARGET_PAGE_MASK, ~0x0, BP_MEM_READ);
    return ldub_phys(addr);
}

static uint32_t watch_mem_readw(void *opaque, target_phys_addr_t addr)
{
    check_watchpoint(addr & ~TARGET_PAGE_MASK, ~0x1, BP_MEM_READ);
    return lduw_phys(addr);
}

static uint32_t watch_mem_readl(void *opaque, target_phys_addr_t addr)
{
    check_watchpoint(addr & ~TARGET_PAGE_MASK, ~0x3, BP_MEM_READ);
    return ldl_phys(addr);
}

static void watch_mem_writeb(void *opaque, target_phys_addr_t addr,
                             uint32_t val)
{
    check_watchpoint(addr & ~TARGET_PAGE_MASK, ~0x0, BP_MEM_WRITE);
    stb_phys(addr, val);
}

static void watch_mem_writew(void *opaque, target_phys_addr_t addr,
                             uint32_t val)
{
    check_watchpoint(addr & ~TARGET_PAGE_MASK, ~0x1, BP_MEM_WRITE);
    stw_phys(addr, val);
}

static void watch_mem_writel(void *opaque, target_phys_addr_t addr,
                             uint32_t val)
{
    check_watchpoint(addr & ~TARGET_PAGE_MASK, ~0x3, BP_MEM_WRITE);
    stl_phys(addr, val);
}

static CPUReadMemoryFunc * const watch_mem_read[3] = {
    watch_mem_readb,
    watch_mem_readw,
    watch_mem_readl,
};

static CPUWriteMemoryFunc * const watch_mem_write[3] = {
    watch_mem_writeb,
    watch_mem_writew,
    watch_mem_writel,
};

static inline uint32_t subpage_readlen (subpage_t *mmio,
                                        target_phys_addr_t addr,
                                        unsigned int len)
{
    unsigned int idx = SUBPAGE_IDX(addr);
#if defined(DEBUG_SUBPAGE)
    printf("%s: subpage %p len %d addr " TARGET_FMT_plx " idx %d\n", __func__,
           mmio, len, addr, idx);
#endif

    addr += mmio->region_offset[idx];
    idx = mmio->sub_io_index[idx];
    return io_mem_read[idx][len](io_mem_opaque[idx], addr);
}

static inline void subpage_writelen (subpage_t *mmio, target_phys_addr_t addr,
                                     uint32_t value, unsigned int len)
{
    unsigned int idx = SUBPAGE_IDX(addr);
#if defined(DEBUG_SUBPAGE)
    printf("%s: subpage %p len %d addr " TARGET_FMT_plx " idx %d value %08x\n",
           __func__, mmio, len, addr, idx, value);
#endif

    addr += mmio->region_offset[idx];
    idx = mmio->sub_io_index[idx];
    io_mem_write[idx][len](io_mem_opaque[idx], addr, value);
}

static uint32_t subpage_readb (void *opaque, target_phys_addr_t addr)
{
    return subpage_readlen(opaque, addr, 0);
}

static void subpage_writeb (void *opaque, target_phys_addr_t addr,
                            uint32_t value)
{
    subpage_writelen(opaque, addr, value, 0);
}

static uint32_t subpage_readw (void *opaque, target_phys_addr_t addr)
{
    return subpage_readlen(opaque, addr, 1);
}

static void subpage_writew (void *opaque, target_phys_addr_t addr,
                            uint32_t value)
{
    subpage_writelen(opaque, addr, value, 1);
}

static uint32_t subpage_readl (void *opaque, target_phys_addr_t addr)
{
    return subpage_readlen(opaque, addr, 2);
}

static void subpage_writel (void *opaque, target_phys_addr_t addr,
                            uint32_t value)
{
    subpage_writelen(opaque, addr, value, 2);
}

static CPUReadMemoryFunc * const subpage_read[] = {
    &subpage_readb,
    &subpage_readw,
    &subpage_readl,
};

static CPUWriteMemoryFunc * const subpage_write[] = {
    &subpage_writeb,
    &subpage_writew,
    &subpage_writel,
};

static int subpage_register (subpage_t *mmio, uint32_t start, uint32_t end,
                             ram_addr_t memory, ram_addr_t region_offset)
{
    int idx, eidx;

    if (start >= TARGET_PAGE_SIZE || end >= TARGET_PAGE_SIZE)
        return -1;
    idx = SUBPAGE_IDX(start);
    eidx = SUBPAGE_IDX(end);
#if defined(DEBUG_SUBPAGE)
    printf("%s: %p start %08x end %08x idx %08x eidx %08x mem %ld\n", __func__,
           mmio, start, end, idx, eidx, memory);
#endif
    if ((memory & ~TARGET_PAGE_MASK) == IO_MEM_RAM)
        memory = IO_MEM_UNASSIGNED;
    memory = (memory >> IO_MEM_SHIFT) & (IO_MEM_NB_ENTRIES - 1);
    for (; idx <= eidx; idx++) {
        mmio->sub_io_index[idx] = memory;
        mmio->region_offset[idx] = region_offset;
    }

    return 0;
}

static subpage_t *subpage_init (target_phys_addr_t base, ram_addr_t *phys,
                                ram_addr_t orig_memory,
                                ram_addr_t region_offset)
{
    subpage_t *mmio;
    int subpage_memory;

    mmio = g_malloc0(sizeof(subpage_t));

    mmio->base = base;
    subpage_memory = cpu_register_io_memory(subpage_read, subpage_write, mmio,
                                            DEVICE_NATIVE_ENDIAN);
#if defined(DEBUG_SUBPAGE)
    printf("%s: %p base " TARGET_FMT_plx " len %08x %d\n", __func__,
           mmio, base, TARGET_PAGE_SIZE, subpage_memory);
#endif
    *phys = subpage_memory | IO_MEM_SUBPAGE;
    subpage_register(mmio, 0, TARGET_PAGE_SIZE-1, orig_memory, region_offset);

    return mmio;
}

static int get_free_io_mem_idx(void)
{
    int i;

    for (i = 0; i<IO_MEM_NB_ENTRIES; i++)
        if (!io_mem_used[i]) {
            io_mem_used[i] = 1;
            return i;
        }
    fprintf(stderr, "RAN out out io_mem_idx, max %d !\n", IO_MEM_NB_ENTRIES);
    return -1;
}

/*
 * Usually, devices operate in little endian mode. There are devices out
 * there that operate in big endian too. Each device gets byte swapped
 * mmio if plugged onto a CPU that does the other endianness.
 *
 * CPU          Device           swap?
 *
 * little       little           no
 * little       big              yes
 * big          little           yes
 * big          big              no
 */

typedef struct SwapEndianContainer {
    CPUReadMemoryFunc *read[3];
    CPUWriteMemoryFunc *write[3];
    void *opaque;
} SwapEndianContainer;

static uint32_t swapendian_mem_readb (void *opaque, target_phys_addr_t addr)
{
    uint32_t val;
    SwapEndianContainer *c = opaque;
    val = c->read[0](c->opaque, addr);
    return val;
}

static uint32_t swapendian_mem_readw(void *opaque, target_phys_addr_t addr)
{
    uint32_t val;
    SwapEndianContainer *c = opaque;
    val = bswap16(c->read[1](c->opaque, addr));
    return val;
}

static uint32_t swapendian_mem_readl(void *opaque, target_phys_addr_t addr)
{
    uint32_t val;
    SwapEndianContainer *c = opaque;
    val = bswap32(c->read[2](c->opaque, addr));
    return val;
}

static CPUReadMemoryFunc * const swapendian_readfn[3]={
    swapendian_mem_readb,
    swapendian_mem_readw,
    swapendian_mem_readl
};

static void swapendian_mem_writeb(void *opaque, target_phys_addr_t addr,
                                  uint32_t val)
{
    SwapEndianContainer *c = opaque;
    c->write[0](c->opaque, addr, val);
}

static void swapendian_mem_writew(void *opaque, target_phys_addr_t addr,
                                  uint32_t val)
{
    SwapEndianContainer *c = opaque;
    c->write[1](c->opaque, addr, bswap16(val));
}

static void swapendian_mem_writel(void *opaque, target_phys_addr_t addr,
                                  uint32_t val)
{
    SwapEndianContainer *c = opaque;
    c->write[2](c->opaque, addr, bswap32(val));
}

static CPUWriteMemoryFunc * const swapendian_writefn[3]={
    swapendian_mem_writeb,
    swapendian_mem_writew,
    swapendian_mem_writel
};

static void swapendian_init(int io_index)
{
    SwapEndianContainer *c = g_malloc(sizeof(SwapEndianContainer));
    int i;

    /* Swap mmio for big endian targets */
    c->opaque = io_mem_opaque[io_index];
    for (i = 0; i < 3; i++) {
        c->read[i] = io_mem_read[io_index][i];
        c->write[i] = io_mem_write[io_index][i];

        io_mem_read[io_index][i] = swapendian_readfn[i];
        io_mem_write[io_index][i] = swapendian_writefn[i];
    }
    io_mem_opaque[io_index] = c;
}

static void swapendian_del(int io_index)
{
    if (io_mem_read[io_index][0] == swapendian_readfn[0]) {
        g_free(io_mem_opaque[io_index]);
    }
}

/* mem_read and mem_write are arrays of functions containing the
   function to access byte (index 0), word (index 1) and dword (index
   2). Functions can be omitted with a NULL function pointer.
   If io_index is non zero, the corresponding io zone is
   modified. If it is zero, a new io zone is allocated. The return
   value can be used with cpu_register_physical_memory(). (-1) is
   returned if error. */
static int cpu_register_io_memory_fixed(int io_index,
                                        CPUReadMemoryFunc * const *mem_read,
                                        CPUWriteMemoryFunc * const *mem_write,
                                        void *opaque, enum device_endian endian)
{
    int i;

    if (io_index <= 0) {
        io_index = get_free_io_mem_idx();
        if (io_index == -1)
            return io_index;
    } else {
        io_index >>= IO_MEM_SHIFT;
        if (io_index >= IO_MEM_NB_ENTRIES)
            return -1;
    }

    for (i = 0; i < 3; ++i) {
        io_mem_read[io_index][i]
            = (mem_read[i] ? mem_read[i] : unassigned_mem_read[i]);
    }
    for (i = 0; i < 3; ++i) {
        io_mem_write[io_index][i]
            = (mem_write[i] ? mem_write[i] : unassigned_mem_write[i]);
    }
    io_mem_opaque[io_index] = opaque;

    switch (endian) {
    case DEVICE_BIG_ENDIAN:
#ifndef TARGET_WORDS_BIGENDIAN
        swapendian_init(io_index);
#endif
        break;
    case DEVICE_LITTLE_ENDIAN:
#ifdef TARGET_WORDS_BIGENDIAN
        swapendian_init(io_index);
#endif
        break;
    case DEVICE_NATIVE_ENDIAN:
    default:
        break;
    }

    return (io_index << IO_MEM_SHIFT);
}

int cpu_register_io_memory(CPUReadMemoryFunc * const *mem_read,
                           CPUWriteMemoryFunc * const *mem_write,
                           void *opaque, enum device_endian endian)
{
    return cpu_register_io_memory_fixed(0, mem_read, mem_write, opaque, endian);
}

void cpu_unregister_io_memory(int io_table_address)
{
    int i;
    int io_index = io_table_address >> IO_MEM_SHIFT;

    swapendian_del(io_index);

    for (i=0;i < 3; i++) {
        io_mem_read[io_index][i] = unassigned_mem_read[i];
        io_mem_write[io_index][i] = unassigned_mem_write[i];
    }
    io_mem_opaque[io_index] = NULL;
    io_mem_used[io_index] = 0;
}

static void io_mem_init(void)
{
    int i;

    cpu_register_io_memory_fixed(IO_MEM_ROM, error_mem_read,
                                 unassigned_mem_write, NULL,
                                 DEVICE_NATIVE_ENDIAN);
    cpu_register_io_memory_fixed(IO_MEM_UNASSIGNED, unassigned_mem_read,
                                 unassigned_mem_write, NULL,
                                 DEVICE_NATIVE_ENDIAN);
    cpu_register_io_memory_fixed(IO_MEM_NOTDIRTY, error_mem_read,
                                 notdirty_mem_write, NULL,
                                 DEVICE_NATIVE_ENDIAN);
    for (i=0; i<5; i++)
        io_mem_used[i] = 1;

    io_mem_watch = cpu_register_io_memory(watch_mem_read,
                                          watch_mem_write, NULL,
                                          DEVICE_NATIVE_ENDIAN);
}

static void memory_map_init(void)
{
    system_memory = g_malloc(sizeof(*system_memory));
    memory_region_init(system_memory, "system", INT64_MAX);
    set_system_memory_map(system_memory);

    system_io = g_malloc(sizeof(*system_io));
    memory_region_init(system_io, "io", 65536);
    set_system_io_map(system_io);
}

MemoryRegion *get_system_memory(void)
{
    return system_memory;
}

MemoryRegion *get_system_io(void)
{
    return system_io;
}

#endif /* !defined(CONFIG_USER_ONLY) */

/* physical memory access (slow version, mainly for debug) */
#if defined(CONFIG_USER_ONLY)
int cpu_memory_rw_debug(CPUState *env, target_ulong addr,
                        uint8_t *buf, int len, int is_write)
{
    int l, flags;
    target_ulong page;
    void * p;

    while (len > 0) {
        page = addr & TARGET_PAGE_MASK;
        l = (page + TARGET_PAGE_SIZE) - addr;
        if (l > len)
            l = len;
        flags = page_get_flags(page);
        if (!(flags & PAGE_VALID))
            return -1;
        if (is_write) {
            if (!(flags & PAGE_WRITE))
                return -1;
            /* XXX: this code should not depend on lock_user */
            if (!(p = lock_user(VERIFY_WRITE, addr, l, 0)))
                return -1;
            memcpy(p, buf, l);
            unlock_user(p, addr, l);
        } else {
            if (!(flags & PAGE_READ))
                return -1;
            /* XXX: this code should not depend on lock_user */
            if (!(p = lock_user(VERIFY_READ, addr, l, 1)))
                return -1;
            memcpy(buf, p, l);
            unlock_user(p, addr, 0);
        }
        len -= l;
        buf += l;
        addr += l;
    }
    return 0;
}

#else
void cpu_physical_memory_rw(target_phys_addr_t addr, uint8_t *buf,
                            int len, int is_write)
{
    int l, io_index;
    uint8_t *ptr;
    uint32_t val;
    target_phys_addr_t page;
    ram_addr_t pd;
    PhysPageDesc *p;

    while (len > 0) {
        page = addr & TARGET_PAGE_MASK;
        l = (page + TARGET_PAGE_SIZE) - addr;
        if (l > len)
            l = len;
        p = phys_page_find(page >> TARGET_PAGE_BITS);
        if (!p) {
            pd = IO_MEM_UNASSIGNED;
        } else {
            pd = p->phys_offset;
        }

        if (is_write) {
            if ((pd & ~TARGET_PAGE_MASK) != IO_MEM_RAM) {
                target_phys_addr_t addr1 = addr;
                io_index = (pd >> IO_MEM_SHIFT) & (IO_MEM_NB_ENTRIES - 1);
                if (p)
                    addr1 = (addr & ~TARGET_PAGE_MASK) + p->region_offset;
                /* XXX: could force cpu_single_env to NULL to avoid
                   potential bugs */
                if (l >= 4 && ((addr1 & 3) == 0)) {
                    /* 32 bit write access */
                    val = ldl_p(buf);
                    io_mem_write[io_index][2](io_mem_opaque[io_index], addr1, val);
                    l = 4;
                } else if (l >= 2 && ((addr1 & 1) == 0)) {
                    /* 16 bit write access */
                    val = lduw_p(buf);
                    io_mem_write[io_index][1](io_mem_opaque[io_index], addr1, val);
                    l = 2;
                } else {
                    /* 8 bit write access */
                    val = ldub_p(buf);
                    io_mem_write[io_index][0](io_mem_opaque[io_index], addr1, val);
                    l = 1;
                }
            } else {
                ram_addr_t addr1;
                addr1 = (pd & TARGET_PAGE_MASK) + (addr & ~TARGET_PAGE_MASK);
                /* RAM case */
		
		if(unlikely(mthread)){
		
                ptr = qemu_get_ram_ptr(addr1);
                memcpy(ptr, buf, l);
		mbarrier(); // TLC
		
                if (!cpu_physical_memory_is_dirty(addr1)) {
                    /* invalidate code */
                    tb_invalidate_phys_page_range(addr1, addr1 + l, 0);
                    /* set dirty bit */
                    cpu_physical_memory_set_dirty_flags(
                        addr1, (0xff & ~CODE_DIRTY_FLAG));
                }
		
		}
		else{ // !mthread
		
                ptr = qemu_get_ram_ptr(addr1);
                memcpy(ptr, buf, l);
		
                if (!cpu_physical_memory_is_dirty(addr1)) {
                    /* invalidate code */
                    tb_invalidate_phys_page_range(addr1, addr1 + l, 0);
                    /* set dirty bit */
                    cpu_physical_memory_set_dirty_flags(
                        addr1, (0xff & ~CODE_DIRTY_FLAG));
                }
		
		}
		/* qemu doesn't execute guest code directly, but kvm does
		   therefore flush instruction caches */
		if (kvm_enabled())
		    flush_icache_range((unsigned long)ptr,
				       ((unsigned long)ptr)+l);
                qemu_put_ram_ptr(ptr);
            }
        } else {
            if ((pd & ~TARGET_PAGE_MASK) > IO_MEM_ROM &&
                !(pd & IO_MEM_ROMD)) {
                target_phys_addr_t addr1 = addr;
                /* I/O case */
                io_index = (pd >> IO_MEM_SHIFT) & (IO_MEM_NB_ENTRIES - 1);
                if (p)
                    addr1 = (addr & ~TARGET_PAGE_MASK) + p->region_offset;
                if (l >= 4 && ((addr1 & 3) == 0)) {
                    /* 32 bit read access */
                    val = io_mem_read[io_index][2](io_mem_opaque[io_index], addr1);
                    stl_p(buf, val);
                    l = 4;
                } else if (l >= 2 && ((addr1 & 1) == 0)) {
                    /* 16 bit read access */
                    val = io_mem_read[io_index][1](io_mem_opaque[io_index], addr1);
                    stw_p(buf, val);
                    l = 2;
                } else {
                    /* 8 bit read access */
                    val = io_mem_read[io_index][0](io_mem_opaque[io_index], addr1);
                    stb_p(buf, val);
                    l = 1;
                }
            } else {
                /* RAM case */
                ptr = qemu_get_ram_ptr(pd & TARGET_PAGE_MASK);
                memcpy(buf, ptr + (addr & ~TARGET_PAGE_MASK), l);
                qemu_put_ram_ptr(ptr);
            }
        }
        len -= l;
        buf += l;
        addr += l;
    }
}

/* used for ROM loading : can write in RAM and ROM */
void cpu_physical_memory_write_rom(target_phys_addr_t addr,
                                   const uint8_t *buf, int len)
{
    int l;
    uint8_t *ptr;
    target_phys_addr_t page;
    unsigned long pd;
    PhysPageDesc *p;

    while (len > 0) {
        page = addr & TARGET_PAGE_MASK;
        l = (page + TARGET_PAGE_SIZE) - addr;
        if (l > len)
            l = len;
        p = phys_page_find(page >> TARGET_PAGE_BITS);
        if (!p) {
            pd = IO_MEM_UNASSIGNED;
        } else {
            pd = p->phys_offset;
        }

        if ((pd & ~TARGET_PAGE_MASK) != IO_MEM_RAM &&
            (pd & ~TARGET_PAGE_MASK) != IO_MEM_ROM &&
            !(pd & IO_MEM_ROMD)) {
            /* do nothing */
        } else {
            unsigned long addr1;
            addr1 = (pd & TARGET_PAGE_MASK) + (addr & ~TARGET_PAGE_MASK);
            /* ROM/RAM case */
            ptr = qemu_get_ram_ptr(addr1);
            memcpy(ptr, buf, l);
            qemu_put_ram_ptr(ptr);
        }
        len -= l;
        buf += l;
        addr += l;
    }
}

typedef struct {
    void *buffer;
    target_phys_addr_t addr;
    target_phys_addr_t len;
} BounceBuffer;

static BounceBuffer bounce;

typedef struct MapClient {
    void *opaque;
    void (*callback)(void *opaque);
    QLIST_ENTRY(MapClient) link;
} MapClient;

static QLIST_HEAD(map_client_list, MapClient) map_client_list
    = QLIST_HEAD_INITIALIZER(map_client_list);

void *cpu_register_map_client(void *opaque, void (*callback)(void *opaque))
{
    MapClient *client = g_malloc(sizeof(*client));

    client->opaque = opaque;
    client->callback = callback;
    QLIST_INSERT_HEAD(&map_client_list, client, link);
    return client;
}

void cpu_unregister_map_client(void *_client)
{
    MapClient *client = (MapClient *)_client;

    QLIST_REMOVE(client, link);
    g_free(client);
}

static void cpu_notify_map_clients(void)
{
    MapClient *client;

    while (!QLIST_EMPTY(&map_client_list)) {
        client = QLIST_FIRST(&map_client_list);
        client->callback(client->opaque);
        cpu_unregister_map_client(client);
    }
}

/* Map a physical memory region into a host virtual address.
 * May map a subset of the requested range, given by and returned in *plen.
 * May return NULL if resources needed to perform the mapping are exhausted.
 * Use only for reads OR writes - not for read-modify-write operations.
 * Use cpu_register_map_client() to know when retrying the map operation is
 * likely to succeed.
 */
void *cpu_physical_memory_map(target_phys_addr_t addr,
                              target_phys_addr_t *plen,
                              int is_write)
{
    target_phys_addr_t len = *plen;
    target_phys_addr_t todo = 0;
    int l;
    target_phys_addr_t page;
    unsigned long pd;
    PhysPageDesc *p;
    ram_addr_t raddr = RAM_ADDR_MAX;
    ram_addr_t rlen;
    void *ret;

    while (len > 0) {
        page = addr & TARGET_PAGE_MASK;
        l = (page + TARGET_PAGE_SIZE) - addr;
        if (l > len)
            l = len;
        p = phys_page_find(page >> TARGET_PAGE_BITS);
        if (!p) {
            pd = IO_MEM_UNASSIGNED;
        } else {
            pd = p->phys_offset;
        }

        if ((pd & ~TARGET_PAGE_MASK) != IO_MEM_RAM) {
            if (todo || bounce.buffer) {
                break;
            }
            bounce.buffer = qemu_memalign(TARGET_PAGE_SIZE, TARGET_PAGE_SIZE);
            bounce.addr = addr;
            bounce.len = l;
            if (!is_write) {
                cpu_physical_memory_read(addr, bounce.buffer, l);
            }

            *plen = l;
            return bounce.buffer;
        }
        if (!todo) {
            raddr = (pd & TARGET_PAGE_MASK) + (addr & ~TARGET_PAGE_MASK);
        }

        len -= l;
        addr += l;
        todo += l;
    }
    rlen = todo;
    ret = qemu_ram_ptr_length(raddr, &rlen);
    *plen = rlen;
    return ret;
}

/* Unmaps a memory region previously mapped by cpu_physical_memory_map().
 * Will also mark the memory as dirty if is_write == 1.  access_len gives
 * the amount of memory that was actually read or written by the caller.
 */
void cpu_physical_memory_unmap(void *buffer, target_phys_addr_t len,
                               int is_write, target_phys_addr_t access_len)
{
    if (buffer != bounce.buffer) {
        if (is_write) {
            ram_addr_t addr1 = qemu_ram_addr_from_host_nofail(buffer);
            while (access_len) {
                unsigned l;
                l = TARGET_PAGE_SIZE;
                if (l > access_len)
                    l = access_len;
                if (!cpu_physical_memory_is_dirty(addr1)) {
                    /* invalidate code */
                    tb_invalidate_phys_page_range(addr1, addr1 + l, 0);
                    /* set dirty bit */
                    cpu_physical_memory_set_dirty_flags(
                        addr1, (0xff & ~CODE_DIRTY_FLAG));
                }
                addr1 += l;
                access_len -= l;
            }
        }
        if (xen_enabled()) {
            xen_invalidate_map_cache_entry(buffer);
        }
        return;
    }
    if (is_write) {
        cpu_physical_memory_write(bounce.addr, bounce.buffer, access_len);
    }
    qemu_vfree(bounce.buffer);
    bounce.buffer = NULL;
    cpu_notify_map_clients();
}

/* warning: addr must be aligned */
static inline uint32_t ldl_phys_internal(target_phys_addr_t addr,
                                         enum device_endian endian)
{
    int io_index;
    uint8_t *ptr;
    uint32_t val;
    unsigned long pd;
    PhysPageDesc *p;

    p = phys_page_find(addr >> TARGET_PAGE_BITS);
    if (!p) {
        pd = IO_MEM_UNASSIGNED;
    } else {
        pd = p->phys_offset;
    }

    if ((pd & ~TARGET_PAGE_MASK) > IO_MEM_ROM &&
        !(pd & IO_MEM_ROMD)) {
        /* I/O case */
        io_index = (pd >> IO_MEM_SHIFT) & (IO_MEM_NB_ENTRIES - 1);
        if (p)
            addr = (addr & ~TARGET_PAGE_MASK) + p->region_offset;
        val = io_mem_read[io_index][2](io_mem_opaque[io_index], addr);
#if defined(TARGET_WORDS_BIGENDIAN)
        if (endian == DEVICE_LITTLE_ENDIAN) {
            val = bswap32(val);
        }
#else
        if (endian == DEVICE_BIG_ENDIAN) {
            val = bswap32(val);
        }
#endif
    } else {
        /* RAM case */
        ptr = qemu_get_ram_ptr(pd & TARGET_PAGE_MASK) +
            (addr & ~TARGET_PAGE_MASK);
        switch (endian) {
        case DEVICE_LITTLE_ENDIAN:
            val = ldl_le_p(ptr);
            break;
        case DEVICE_BIG_ENDIAN:
            val = ldl_be_p(ptr);
            break;
        default:
            val = ldl_p(ptr);
            break;
        }
    }
    return val;
}

uint32_t ldl_phys(target_phys_addr_t addr)
{
    return ldl_phys_internal(addr, DEVICE_NATIVE_ENDIAN);
}

uint32_t ldl_le_phys(target_phys_addr_t addr)
{
    return ldl_phys_internal(addr, DEVICE_LITTLE_ENDIAN);
}

uint32_t ldl_be_phys(target_phys_addr_t addr)
{
    return ldl_phys_internal(addr, DEVICE_BIG_ENDIAN);
}

/* warning: addr must be aligned */
static inline uint64_t ldq_phys_internal(target_phys_addr_t addr,
                                         enum device_endian endian)
{
    int io_index;
    uint8_t *ptr;
    uint64_t val;
    unsigned long pd;
    PhysPageDesc *p;

    p = phys_page_find(addr >> TARGET_PAGE_BITS);
    if (!p) {
        pd = IO_MEM_UNASSIGNED;
    } else {
        pd = p->phys_offset;
    }

    if ((pd & ~TARGET_PAGE_MASK) > IO_MEM_ROM &&
        !(pd & IO_MEM_ROMD)) {
        /* I/O case */
        io_index = (pd >> IO_MEM_SHIFT) & (IO_MEM_NB_ENTRIES - 1);
        if (p)
            addr = (addr & ~TARGET_PAGE_MASK) + p->region_offset;

        /* XXX This is broken when device endian != cpu endian.
               Fix and add "endian" variable check */
#ifdef TARGET_WORDS_BIGENDIAN
        val = (uint64_t)io_mem_read[io_index][2](io_mem_opaque[io_index], addr) << 32;
        val |= io_mem_read[io_index][2](io_mem_opaque[io_index], addr + 4);
#else
        val = io_mem_read[io_index][2](io_mem_opaque[io_index], addr);
        val |= (uint64_t)io_mem_read[io_index][2](io_mem_opaque[io_index], addr + 4) << 32;
#endif
    } else {
        /* RAM case */
        ptr = qemu_get_ram_ptr(pd & TARGET_PAGE_MASK) +
            (addr & ~TARGET_PAGE_MASK);
        switch (endian) {
        case DEVICE_LITTLE_ENDIAN:
            val = ldq_le_p(ptr);
            break;
        case DEVICE_BIG_ENDIAN:
            val = ldq_be_p(ptr);
            break;
        default:
            val = ldq_p(ptr);
            break;
        }
    }
    return val;
}

uint64_t ldq_phys(target_phys_addr_t addr)
{
    return ldq_phys_internal(addr, DEVICE_NATIVE_ENDIAN);
}

uint64_t ldq_le_phys(target_phys_addr_t addr)
{
    return ldq_phys_internal(addr, DEVICE_LITTLE_ENDIAN);
}

uint64_t ldq_be_phys(target_phys_addr_t addr)
{
    return ldq_phys_internal(addr, DEVICE_BIG_ENDIAN);
}

/* XXX: optimize */
uint32_t ldub_phys(target_phys_addr_t addr)
{
    uint8_t val;
    cpu_physical_memory_read(addr, &val, 1);
    return val;
}

/* warning: addr must be aligned */
static inline uint32_t lduw_phys_internal(target_phys_addr_t addr,
                                          enum device_endian endian)
{
    int io_index;
    uint8_t *ptr;
    uint64_t val;
    unsigned long pd;
    PhysPageDesc *p;

    p = phys_page_find(addr >> TARGET_PAGE_BITS);
    if (!p) {
        pd = IO_MEM_UNASSIGNED;
    } else {
        pd = p->phys_offset;
    }

    if ((pd & ~TARGET_PAGE_MASK) > IO_MEM_ROM &&
        !(pd & IO_MEM_ROMD)) {
        /* I/O case */
        io_index = (pd >> IO_MEM_SHIFT) & (IO_MEM_NB_ENTRIES - 1);
        if (p)
            addr = (addr & ~TARGET_PAGE_MASK) + p->region_offset;
        val = io_mem_read[io_index][1](io_mem_opaque[io_index], addr);
#if defined(TARGET_WORDS_BIGENDIAN)
        if (endian == DEVICE_LITTLE_ENDIAN) {
            val = bswap16(val);
        }
#else
        if (endian == DEVICE_BIG_ENDIAN) {
            val = bswap16(val);
        }
#endif
    } else {
        /* RAM case */
        ptr = qemu_get_ram_ptr(pd & TARGET_PAGE_MASK) +
            (addr & ~TARGET_PAGE_MASK);
        switch (endian) {
        case DEVICE_LITTLE_ENDIAN:
            val = lduw_le_p(ptr);
            break;
        case DEVICE_BIG_ENDIAN:
            val = lduw_be_p(ptr);
            break;
        default:
            val = lduw_p(ptr);
            break;
        }
    }
    return val;
}

uint32_t lduw_phys(target_phys_addr_t addr)
{
    return lduw_phys_internal(addr, DEVICE_NATIVE_ENDIAN);
}

uint32_t lduw_le_phys(target_phys_addr_t addr)
{
    return lduw_phys_internal(addr, DEVICE_LITTLE_ENDIAN);
}

uint32_t lduw_be_phys(target_phys_addr_t addr)
{
    return lduw_phys_internal(addr, DEVICE_BIG_ENDIAN);
}

/* warning: addr must be aligned. The ram page is not masked as dirty
   and the code inside is not invalidated. It is useful if the dirty
   bits are used to track modified PTEs */
void stl_phys_notdirty(target_phys_addr_t addr, uint32_t val)
{
    int io_index;
    uint8_t *ptr;
    unsigned long pd;
    PhysPageDesc *p;

    p = phys_page_find(addr >> TARGET_PAGE_BITS);
    if (!p) {
        pd = IO_MEM_UNASSIGNED;
    } else {
        pd = p->phys_offset;
    }

    if ((pd & ~TARGET_PAGE_MASK) != IO_MEM_RAM) {
        io_index = (pd >> IO_MEM_SHIFT) & (IO_MEM_NB_ENTRIES - 1);
        if (p)
            addr = (addr & ~TARGET_PAGE_MASK) + p->region_offset;
        io_mem_write[io_index][2](io_mem_opaque[io_index], addr, val);
    } else {
        unsigned long addr1 = (pd & TARGET_PAGE_MASK) + (addr & ~TARGET_PAGE_MASK);
        ptr = qemu_get_ram_ptr(addr1);
        stl_p(ptr, val);

        if (unlikely(in_migration)) {
            if (!cpu_physical_memory_is_dirty(addr1)) {
                /* invalidate code */
                tb_invalidate_phys_page_range(addr1, addr1 + 4, 0);
                /* set dirty bit */
                cpu_physical_memory_set_dirty_flags(
                    addr1, (0xff & ~CODE_DIRTY_FLAG));
            }
        }
    }
}

void stq_phys_notdirty(target_phys_addr_t addr, uint64_t val)
{
    int io_index;
    uint8_t *ptr;
    unsigned long pd;
    PhysPageDesc *p;

    p = phys_page_find(addr >> TARGET_PAGE_BITS);
    if (!p) {
        pd = IO_MEM_UNASSIGNED;
    } else {
        pd = p->phys_offset;
    }

    if ((pd & ~TARGET_PAGE_MASK) != IO_MEM_RAM) {
        io_index = (pd >> IO_MEM_SHIFT) & (IO_MEM_NB_ENTRIES - 1);
        if (p)
            addr = (addr & ~TARGET_PAGE_MASK) + p->region_offset;
#ifdef TARGET_WORDS_BIGENDIAN
        io_mem_write[io_index][2](io_mem_opaque[io_index], addr, val >> 32);
        io_mem_write[io_index][2](io_mem_opaque[io_index], addr + 4, val);
#else
        io_mem_write[io_index][2](io_mem_opaque[io_index], addr, val);
        io_mem_write[io_index][2](io_mem_opaque[io_index], addr + 4, val >> 32);
#endif
    } else {
        ptr = qemu_get_ram_ptr(pd & TARGET_PAGE_MASK) +
            (addr & ~TARGET_PAGE_MASK);
        stq_p(ptr, val);
    }
}

/* warning: addr must be aligned */
static inline void stl_phys_internal(target_phys_addr_t addr, uint32_t val,
                                     enum device_endian endian)
{
    int io_index;
    uint8_t *ptr;
    unsigned long pd;
    PhysPageDesc *p;

    p = phys_page_find(addr >> TARGET_PAGE_BITS);
    if (!p) {
        pd = IO_MEM_UNASSIGNED;
    } else {
        pd = p->phys_offset;
    }

    if ((pd & ~TARGET_PAGE_MASK) != IO_MEM_RAM) {
        io_index = (pd >> IO_MEM_SHIFT) & (IO_MEM_NB_ENTRIES - 1);
        if (p)
            addr = (addr & ~TARGET_PAGE_MASK) + p->region_offset;
#if defined(TARGET_WORDS_BIGENDIAN)
        if (endian == DEVICE_LITTLE_ENDIAN) {
            val = bswap32(val);
        }
#else
        if (endian == DEVICE_BIG_ENDIAN) {
            val = bswap32(val);
        }
#endif
        io_mem_write[io_index][2](io_mem_opaque[io_index], addr, val);
    } else {
        unsigned long addr1;
        addr1 = (pd & TARGET_PAGE_MASK) + (addr & ~TARGET_PAGE_MASK);
        /* RAM case */
        ptr = qemu_get_ram_ptr(addr1);
        switch (endian) {
        case DEVICE_LITTLE_ENDIAN:
            stl_le_p(ptr, val);
            break;
        case DEVICE_BIG_ENDIAN:
            stl_be_p(ptr, val);
            break;
        default:
            stl_p(ptr, val);
            break;
        }
        if (!cpu_physical_memory_is_dirty(addr1)) {
            /* invalidate code */
            tb_invalidate_phys_page_range(addr1, addr1 + 4, 0);
            /* set dirty bit */
            cpu_physical_memory_set_dirty_flags(addr1,
                (0xff & ~CODE_DIRTY_FLAG));
        }
    }
}

void stl_phys(target_phys_addr_t addr, uint32_t val)
{
    stl_phys_internal(addr, val, DEVICE_NATIVE_ENDIAN);
}

void stl_le_phys(target_phys_addr_t addr, uint32_t val)
{
    stl_phys_internal(addr, val, DEVICE_LITTLE_ENDIAN);
}

void stl_be_phys(target_phys_addr_t addr, uint32_t val)
{
    stl_phys_internal(addr, val, DEVICE_BIG_ENDIAN);
}

/* XXX: optimize */
void stb_phys(target_phys_addr_t addr, uint32_t val)
{
    uint8_t v = val;
    cpu_physical_memory_write(addr, &v, 1);
}

/* warning: addr must be aligned */
static inline void stw_phys_internal(target_phys_addr_t addr, uint32_t val,
                                     enum device_endian endian)
{
    int io_index;
    uint8_t *ptr;
    unsigned long pd;
    PhysPageDesc *p;

    p = phys_page_find(addr >> TARGET_PAGE_BITS);
    if (!p) {
        pd = IO_MEM_UNASSIGNED;
    } else {
        pd = p->phys_offset;
    }

    if ((pd & ~TARGET_PAGE_MASK) != IO_MEM_RAM) {
        io_index = (pd >> IO_MEM_SHIFT) & (IO_MEM_NB_ENTRIES - 1);
        if (p)
            addr = (addr & ~TARGET_PAGE_MASK) + p->region_offset;
#if defined(TARGET_WORDS_BIGENDIAN)
        if (endian == DEVICE_LITTLE_ENDIAN) {
            val = bswap16(val);
        }
#else
        if (endian == DEVICE_BIG_ENDIAN) {
            val = bswap16(val);
        }
#endif
        io_mem_write[io_index][1](io_mem_opaque[io_index], addr, val);
    } else {
        unsigned long addr1;
        addr1 = (pd & TARGET_PAGE_MASK) + (addr & ~TARGET_PAGE_MASK);
        /* RAM case */
        ptr = qemu_get_ram_ptr(addr1);
        switch (endian) {
        case DEVICE_LITTLE_ENDIAN:
            stw_le_p(ptr, val);
            break;
        case DEVICE_BIG_ENDIAN:
            stw_be_p(ptr, val);
            break;
        default:
            stw_p(ptr, val);
            break;
        }
        if (!cpu_physical_memory_is_dirty(addr1)) {
            /* invalidate code */
            tb_invalidate_phys_page_range(addr1, addr1 + 2, 0);
            /* set dirty bit */
            cpu_physical_memory_set_dirty_flags(addr1,
                (0xff & ~CODE_DIRTY_FLAG));
        }
    }
}

void stw_phys(target_phys_addr_t addr, uint32_t val)
{
    stw_phys_internal(addr, val, DEVICE_NATIVE_ENDIAN);
}

void stw_le_phys(target_phys_addr_t addr, uint32_t val)
{
    stw_phys_internal(addr, val, DEVICE_LITTLE_ENDIAN);
}

void stw_be_phys(target_phys_addr_t addr, uint32_t val)
{
    stw_phys_internal(addr, val, DEVICE_BIG_ENDIAN);
}

/* XXX: optimize */
void stq_phys(target_phys_addr_t addr, uint64_t val)
{
    val = tswap64(val);
    cpu_physical_memory_write(addr, &val, 8);
}

void stq_le_phys(target_phys_addr_t addr, uint64_t val)
{
    val = cpu_to_le64(val);
    cpu_physical_memory_write(addr, &val, 8);
}

void stq_be_phys(target_phys_addr_t addr, uint64_t val)
{
    val = cpu_to_be64(val);
    cpu_physical_memory_write(addr, &val, 8);
}

/* virtual memory access for debug (includes writing to ROM) */
int cpu_memory_rw_debug(CPUState *env, target_ulong addr,
                        uint8_t *buf, int len, int is_write)
{
    int l;
    target_phys_addr_t phys_addr;
    target_ulong page;

    while (len > 0) {
        page = addr & TARGET_PAGE_MASK;
        phys_addr = cpu_get_phys_page_debug(env, page);
        /* if no physical page mapped, return an error */
        if (phys_addr == -1)
            return -1;
        l = (page + TARGET_PAGE_SIZE) - addr;
        if (l > len)
            l = len;
        phys_addr += (addr & ~TARGET_PAGE_MASK);
        if (is_write)
            cpu_physical_memory_write_rom(phys_addr, buf, l);
        else
            cpu_physical_memory_rw(phys_addr, buf, l, is_write);
        len -= l;
        buf += l;
        addr += l;
    }
    return 0;
}
#endif

/* in deterministic execution mode, instructions doing device I/Os
   must be at the end of the TB */
void cpu_io_recompile(CPUState *env, void *retaddr)
{
    TranslationBlock *tb;
    uint32_t n, cflags;
    target_ulong pc, cs_base;
    uint64_t flags;

    tb = tb_find_pc((unsigned long)retaddr);
    if (!tb) {
        cpu_abort(env, "cpu_io_recompile: could not find TB for pc=%p", 
                  retaddr);
    }
    n = env->icount_decr.u16.low + tb->icount;
    cpu_restore_state(tb, env, (unsigned long)retaddr);
    /* Calculate how many instructions had been executed before the fault
       occurred.  */
    n = n - env->icount_decr.u16.low;
    /* Generate a new TB ending on the I/O insn.  */
    n++;
    /* On MIPS and SH, delay slot instructions can only be restarted if
       they were already the first instruction in the TB.  If this is not
       the first instruction in a TB then re-execute the preceding
       branch.  */
#if defined(TARGET_MIPS)
    if ((env->hflags & MIPS_HFLAG_BMASK) != 0 && n > 1) {
        env->active_tc.PC -= 4;
        env->icount_decr.u16.low++;
        env->hflags &= ~MIPS_HFLAG_BMASK;
    }
#elif defined(TARGET_SH4)
    if ((env->flags & ((DELAY_SLOT | DELAY_SLOT_CONDITIONAL))) != 0
            && n > 1) {
        env->pc -= 2;
        env->icount_decr.u16.low++;
        env->flags &= ~(DELAY_SLOT | DELAY_SLOT_CONDITIONAL);
    }
#endif
    /* This should never happen.  */
    if (n > CF_COUNT_MASK)
        cpu_abort(env, "TB too big during recompile");

    cflags = n | CF_LAST_IO;
    pc = tb->pc;
    cs_base = tb->cs_base;
    flags = tb->flags;
    tb_phys_invalidate(tb, -1);
    /* FIXME: In theory this could raise an exception.  In practice
       we have already translated the block once so it's probably ok.  */
    tb_gen_code(env, pc, cs_base, flags, cflags);
    /* TODO: If env->pc != tb->pc (i.e. the faulting instruction was not
       the first in the TB) then we end up generating a whole new TB and
       repeating the fault, which is horribly inefficient.
       Better would be to execute just this insn uncached, or generate a
       second new TB.  */
    cpu_resume_from_signal(env, NULL);
}

#if !defined(CONFIG_USER_ONLY)

void dump_exec_info(FILE *f, fprintf_function cpu_fprintf)
{
    int i, target_code_size, max_target_code_size;
    int direct_jmp_count, direct_jmp2_count, cross_page;
    TranslationBlock *tb;

    target_code_size = 0;
    max_target_code_size = 0;
    cross_page = 0;
    direct_jmp_count = 0;
    direct_jmp2_count = 0;
    for(i = 0; i < nb_tbs; i++) {
        tb = &tbs[i];
        target_code_size += tb->size;
        if (tb->size > max_target_code_size)
            max_target_code_size = tb->size;
        if (tb->page_addr[1] != -1)
            cross_page++;
        if (tb->tb_next_offset[0] != 0xffff) {
            direct_jmp_count++;
            if (tb->tb_next_offset[1] != 0xffff) {
                direct_jmp2_count++;
            }
        }
    }
    /* XXX: avoid using doubles ? */
    cpu_fprintf(f, "Translation buffer state:\n");
    cpu_fprintf(f, "gen code size       %td/%ld\n",
                code_gen_ptr - code_gen_buffer, code_gen_buffer_max_size);
    cpu_fprintf(f, "TB count            %d/%d\n", 
                nb_tbs, code_gen_max_blocks);
    cpu_fprintf(f, "TB avg target size  %d max=%d bytes\n",
                nb_tbs ? target_code_size / nb_tbs : 0,
                max_target_code_size);
    cpu_fprintf(f, "TB avg host size    %td bytes (expansion ratio: %0.1f)\n",
                nb_tbs ? (code_gen_ptr - code_gen_buffer) / nb_tbs : 0,
                target_code_size ? (double) (code_gen_ptr - code_gen_buffer) / target_code_size : 0);
    cpu_fprintf(f, "cross page TB count %d (%d%%)\n",
            cross_page,
            nb_tbs ? (cross_page * 100) / nb_tbs : 0);
    cpu_fprintf(f, "direct jump count   %d (%d%%) (2 jumps=%d %d%%)\n",
                direct_jmp_count,
                nb_tbs ? (direct_jmp_count * 100) / nb_tbs : 0,
                direct_jmp2_count,
                nb_tbs ? (direct_jmp2_count * 100) / nb_tbs : 0);
    cpu_fprintf(f, "\nStatistics:\n");
    cpu_fprintf(f, "TB flush count      %d\n", tb_flush_count);
    cpu_fprintf(f, "TB invalidate count %d\n", tb_phys_invalidate_count);
    cpu_fprintf(f, "TLB flush count     %d\n", tlb_flush_count);
#ifdef CONFIG_PROFILER
    tcg_dump_info(f, cpu_fprintf);
#endif
}

#define MMUSUFFIX _cmmu
#undef GETPC
#define GETPC() NULL
#define env cpu_single_env
#define SOFTMMU_CODE_ACCESS

#define SHIFT 0
#include "softmmu_template.h"

#define SHIFT 1
#include "softmmu_template.h"

#define SHIFT 2
#include "softmmu_template.h"

#define SHIFT 3
#include "softmmu_template.h"

#undef env

#endif

// TLC: begin 
#include "buffered_file.h"
#include "uthash.h"
#include <unistd.h>

typedef struct addr_buffer_node {
    ram_addr_t addr;
    uint8_t *ptr;
    uint8_t *mem_contents; 
} addr_buffer_t;

typedef struct ram_content_node {
    int start; // starting indices of addr_buffer
    int num;
    uint8_t *memory_buffer; // contents 
} ram_content_t; 

// storage type's values
//#define 	S2_REMOTE_MEMORY	0x01
//#define	S2_LOCAL_FILE		0x02

typedef struct ram_cow_t {
  ram_addr_t ram_addr;
  uint32_t access_count;
  uint8_t storage_type;
  uint8_t *contents;
  UT_hash_handle hh;
} ram_cow_t;

int 		ram_save_to_hash_worker(void);
void 		*page_saver_t(void *args);
void 		addr_buffer_init(void);
addr_buffer_t 	*add_addr_buffer(ram_addr_t addr, uint8_t *p);
void 		*copier_t (void *t);
int 		get_num_cp_threads(void);
void 		copying_pages_in_addr_buffer(void);
int 		ram_save_to_buffer_parallel(void);

int 		put_page_to_hash(ram_addr_t addr, uint8_t *ptr);
int 		tlc_handle_dirty_ram(void);
int 		hash_save_block(QEMUFile *f);

extern uint64_t pages_copied_last_epoch;
extern uint64_t bytes_transferred;
extern uint64_t pages_transferred;

// TLC: worker thread
pthread_mutex_t mutex_save_page;
pthread_mutex_t mutex_theone;
pthread_cond_t  have_dirty;
int		nodirty = 1;
int 		stage2_done = 0;

ram_cow_t *exclude_page_from_hash(ram_addr_t addr);

uint64_t tlc_vm_written_page_count = 0;
uint64_t tlc_vm_written_byte_count = 0;
uint64_t tlc_vm_old_pages = 0;
uint64_t tlc_vm_new_pages = 0;

ram_cow_t 	*ram_cow_buffer = NULL;

uint64_t tlc_vm_hash_table_add = 0;
uint64_t tlc_vm_hash_table_excluded = 0;

addr_buffer_t * addr_buffer;

#define		ADDR_BUFFER_CHUNK	4096
uint64_t 	addr_buffer_size = 0;
uint64_t 	last_addr_buffer = 0;

ram_content_t * ram_contents;

int 		num_cp_threads = 0;
uint32_t	mflag = 0x00010001; // vm_id:num_id 

#define REMOTE_MEM_STORAGE	1
#define LOCAL_MEM_STORAGE	2

// TLC stat remove later...
uint64_t ps_add_hash_page_stat[5] = { 0, 0, 0, 0, 0};

// ports on memory server to transmit new dirty pages
int 		tlc_mserv_base_port = 0;
const char 	*tlc_mserv_ip = NULL;

// TLC INTERVAL
extern int tlc_interval;
extern int tlc_slowdown_interval;

// port and IP for VIC protocol
#include "tlc.h"

int		vic_flag = 0;
int 		vic_port = 0;
const char 	*vic_ip = NULL;
extern int vic_report(char *message);

#define	TLC_OUTGOING_PAGES	1
#define TLC_INCOMING_PAGES	2

extern void *tlc_incoming_t(void *t);
extern int mc_create_connection(int num_conn);
extern int mc_create_connection2(int num_conn);
extern int vic_create_connection(int num_conn);
extern void tlc_incoming_variables_init(void);

// TLC migration configuration parameter strings

extern char *tlc_mserv_ip_str;
extern char *tlc_mserv_base_port_str;
extern char *tlc_interval_str;
extern char *tlc_slowdown_interval_str;
extern char *vic_flag_str;
extern char *vic_ip_str;
extern char *vic_port_str;
extern int  tlc_config(void);

extern int state_transfer_type; 
extern int chkpt_mserv_cnt; 

void live_storage_init(int flag, int num_thr);

void live_storage_init(int flag, int num_thr){
	//int i;
	const char *base_port;
	const char *number_string;
	const char *interval_string;
	const char *slowdown_interval_string;
	const char *vic_flag_string;
	const char *vic_port_string;
	
	if(flag == TLC_INCOMING_PAGES){
		tlc_config();	
	}

	if ((base_port = tlc_mserv_base_port_str)!= NULL){
		tlc_mserv_base_port = atoi(base_port);
	}

        if(tlc_mserv_ip_str != NULL)
		tlc_mserv_ip = tlc_mserv_ip_str;

	// TLC INTERVAL
	if ((interval_string = tlc_interval_str)!= NULL){
		tlc_interval = atoi(interval_string);
	}

	if ((slowdown_interval_string = tlc_slowdown_interval_str)!= NULL){
		tlc_slowdown_interval = atoi(slowdown_interval_string);
	}

	// VIC stuffs
	if ((vic_flag_string = vic_flag_str)!= NULL){
	    if(strncmp(vic_flag_string, "OFF", 3) == 0){
    	        vic_flag = 0;
	    }
	    else{
    	        vic_flag = 1;
	    }

	}
	else{
		vic_flag = 0;
	}

	if ((vic_port_string = vic_port_str)!= NULL){
		vic_port = atoi(vic_port_string);
	}

	if(vic_ip_str != NULL)
		vic_ip = vic_ip_str;

printf(" live_storage_init: vic_flag = %d VIC PORT = %d VIC IP=%s \n", vic_flag, vic_port, vic_ip);
	
	if(flag == TLC_OUTGOING_PAGES){
	    pthread_mutex_init(&mutex_save_page, NULL);
	    pthread_mutex_init(&mutex_theone, NULL);
	    pthread_cond_init(&have_dirty, NULL);

            // For now, we assume chkpt_mserv_cnt == 0 when migrate
            if(chkpt_mserv_cnt == 0){ // TLM ONLY

		mc_create_connection(1);
		mc_create_connection2(1);

		if(vic_flag) vic_create_connection(1);
            }

	}
	else{ // TLC_INCOMING_PAGES
            if(
              (state_transfer_type == TLM)||
              (state_transfer_type == TLM_STANDBY_DST)||
              (state_transfer_type == TLM_STANDBY_SRCDST)||
              (state_transfer_type == TLMC)||
              (state_transfer_type == TLMC_STANDBY_DST)
            ){
		pthread_t mserv_thread;
		int t = 0;
//printf(" live s init() incoming");
		if(vic_flag) vic_create_connection(1);
		//tlc_incoming_variables_init();		
        	pthread_create(&mserv_thread, NULL, tlc_incoming_t, (void *)((long)t));
        	pthread_detach(mserv_thread);
            }
	}
}

static int add_values_to_hash(ram_addr_t addr, uint8_t flag, uint8_t *ptr){
	int ret = 0;
	ram_cow_t *tmp_ptr;	
	// save addr (key's value) to hash
	HASH_FIND_PTR(ram_cow_buffer, &addr, tmp_ptr); 
	if(tmp_ptr){
	    	tmp_ptr->access_count++;
		tmp_ptr->storage_type = flag;
	    	tlc_vm_old_pages++;
		
		if(flag == REMOTE_MEM_STORAGE){
			if (tmp_ptr->contents != NULL){ // was local, now remote
				printf("add_values: addr was local now remote\n");
				fflush(stdout);
				free(tmp_ptr->contents);
				tmp_ptr->contents = NULL;
			}
			ps_add_hash_page_stat[1]++; // TLC stat
			ret = 1;
		}
		else if(flag == LOCAL_MEM_STORAGE){
			if (tmp_ptr->contents == NULL){ // was remote, now local
				printf(" add_values: addr was remote, now local\n");
				fflush(stdout); 
				// we don't delete content from  remote mem here.
				// I don't want to create too much traffic during s2.
				// just allocate memory and move on for now. 
				tmp_ptr->contents = (uint8_t *)malloc(sizeof(uint8_t)*TARGET_PAGE_SIZE);
			}
			memcpy(tmp_ptr->contents, ptr, TARGET_PAGE_SIZE);
			ps_add_hash_page_stat[2]++; // TLC stat
			ret = 2;
		}
		else{
			printf(" add_values: error unknow save flag 1\n");
		}
	}
	else{
	    	// TLC: add a new page 
	    	tmp_ptr = (ram_cow_t *)malloc(sizeof(ram_cow_t));
	    	tmp_ptr->ram_addr = addr; // key
	    	tmp_ptr->access_count = 1;
		tmp_ptr->storage_type = flag;
	    	tlc_vm_new_pages++;
		
		if(flag == REMOTE_MEM_STORAGE){
			tmp_ptr->contents = NULL;
			ps_add_hash_page_stat[3]++; // TLC stat
			ret = 3;
		}
		else if(flag == LOCAL_MEM_STORAGE){
			tmp_ptr->contents = (uint8_t *)malloc(sizeof(uint8_t)*TARGET_PAGE_SIZE);
			memcpy(tmp_ptr->contents, ptr, TARGET_PAGE_SIZE);
			ps_add_hash_page_stat[4]++; // TLC stat
			ret = 4;
		}
		else{
			printf(" add_values: error unknow save flag 2\n");
		}

	    	HASH_ADD_PTR(ram_cow_buffer, ram_addr, tmp_ptr);
	    	tlc_vm_hash_table_add++;
	}
    	return ret;
}

extern int mc_set(const void *mkey, size_t mkey_size, const void *buf, size_t buf_size);
extern int mc_set2(const void *mkey, size_t mkey_size, const void *buf, size_t buf_size);
extern int mserv_set(const void *mkey, size_t mkey_size, const void *buf, size_t buf_size);
extern int s22_mig_trans_failure;
extern int s3_mig_trans_failure;

int *trans_err_flags;
int tlc_pages_sent_mserv = 0;

int put_page_to_hash(ram_addr_t addr, uint8_t *ptr){
	uint64_t mkey = addr; 
	size_t   key_length = sizeof(uint64_t); 
	uint8_t  *values = ptr;
	size_t   value_length = sizeof(uint8_t)*TARGET_PAGE_SIZE;
	//ram_cow_t *tmp_ptr;
	//int8_t   *content = NULL;
	int ret;

//printf("ppth: sending Key %" PRIx64 " \n",mkey);
//fflush(stdout);
        if(
           (state_transfer_type == TLM)||
           (state_transfer_type == TLM_STANDBY_DST)||
           (state_transfer_type == TLM_STANDBY_SRCDST)||
           (state_transfer_type == TLMC)||
           (state_transfer_type == TLMC_STANDBY_DST)
          ){
	   ret= mc_set((const char *) &mkey, key_length, (const char *) values, value_length);
	   //ret= mserv_set((const char *) &mkey, key_length, (const char *) values, value_length);
        }
        else if(
           (state_transfer_type == TLC_EXEC)||
           (state_transfer_type == TLC_TCP_STANDBY_DST)
        ){
	   ret= mserv_set((const char *) &mkey, key_length, (const char *) values, value_length);
        }
        else{
	   printf("ppth: unknown state transfer type  = %d \n", state_transfer_type);
           ret = -1;
           goto out_putpage;
        }
	
	if (ret < 0){
		printf("ppth: Couldn't store key:ret = %d \n", ret);
                // abort migration if fails
		//add_values_to_hash(addr, LOCAL_MEM_STORAGE, ptr);
		//ret = 0;
                s22_mig_trans_failure = 1;
                ret = -1; // error
                goto out_putpage;
	}
	else{
//printf("ppth: Key %" PRIx64 " sent to mserver successfully\n",mkey);
//fflush(stdout);
	// remove later
		//add_values_to_hash(addr, REMOTE_MEM_STORAGE, NULL); 
		tlc_pages_sent_mserv++;
		//add_values_to_hash(addr, LOCAL_MEM_STORAGE, ptr);  
    		ret = 1;
	}
	//ret = 0; // remove later (flag local store for now)	
	
	// TLC: increase count on writen pages
	tlc_vm_written_page_count++;
	tlc_vm_written_byte_count += TARGET_PAGE_SIZE;
out_putpage:    	
	return ret;
}

#define HASH_EXCLUSION_FLAG 	0x01

ram_cow_t *exclude_page_from_hash(ram_addr_t addr)
{
	ram_cow_t *tmp_ptr;

	// TLC: find page in the hash table
	HASH_FIND_PTR(ram_cow_buffer, &addr, tmp_ptr); 
	if(tmp_ptr){
	    tmp_ptr->ram_addr |= HASH_EXCLUSION_FLAG;	    
	    tlc_vm_hash_table_excluded++;	     
	}
	else{
	    tmp_ptr = NULL;
	}	
	return tmp_ptr;
}

#define RAM_SAVE_FLAG_FULL     0x01 /* Obsolete, not used anymore */
#define RAM_SAVE_FLAG_COMPRESS 0x02
#define RAM_SAVE_FLAG_MEM_SIZE 0x04
#define RAM_SAVE_FLAG_PAGE     0x08
#define RAM_SAVE_FLAG_EOS      0x10
#define RAM_SAVE_FLAG_CONTINUE 0x20
// TLC
#define RAM_SAVE_FLAG_SYNC_STATE      0x40

static int is_dup_page(uint8_t *page, uint8_t ch)
{
    uint32_t val = ch << 24 | ch << 16 | ch << 8 | ch;
    uint32_t *array = (uint32_t *)page;
    int i;

    for (i = 0; i < (TARGET_PAGE_SIZE / 4); i++) {
        if (array[i] != val) {
            return 0;
        }
    }

    return 1;
}

extern int tlc_ram_section_id;
#define QEMU_VM_FILE_MAGIC           0x5145564d
#define QEMU_VM_FILE_VERSION_COMPAT  0x00000002
#define QEMU_VM_FILE_VERSION         0x00000003

#define QEMU_VM_EOF                  0x00
#define QEMU_VM_SECTION_START        0x01
#define QEMU_VM_SECTION_PART         0x02
#define QEMU_VM_SECTION_END          0x03
#define QEMU_VM_SECTION_FULL         0x04
#define QEMU_VM_SUBSECTION           0x05

extern char chkpt_recovery_inst_str[MAX_TLC_MSERV][INST_STR_LEN];

int hash_save_block(QEMUFile *f)
{
    int j;
    ram_addr_t current_addr = 0;
    //uint64_t   r_addr;
    //uint32_t   r_access_cnt;
    uint8_t *p;

    ram_cow_t *tmp_ptr;
    
    int num_hash_freed = 0;
    //int num_hash_contents_freed = 0;
    int num_ram_contents_freed = 0;

    int num_skip_hash_block = 0;
    	int num_skip_remote_mem = 0;
	int num_skip_local_mem = 0;
    int saved_blocks = 0;
    	int saved_blocks_remote_mem = 0;
	int saved_blocks_local_mem = 0;
    int saved_buffer_to_chkpt_file = 0;

    DVERYDETAIL{printf("hashsb: in \n");
    fflush(stdout);}
    
// for now, we get mem content from membase and save to file
    qemu_put_byte(f, QEMU_VM_SECTION_PART);
    qemu_put_be32(f, tlc_ram_section_id);
    
    qemu_put_be64(f, current_addr | RAM_SAVE_FLAG_SYNC_STATE);

    if(
      (state_transfer_type == TLM)||
      (state_transfer_type == TLM_STANDBY_DST)||
      (state_transfer_type == TLM_STANDBY_SRCDST)||
      (state_transfer_type == TLMC)||
      (state_transfer_type == TLMC_STANDBY_DST)
    ){
        // do nothing
    }
    else if(
      (state_transfer_type == TLC_EXEC)||
      (state_transfer_type == TLC_TCP_STANDBY_DST)
    ){
        int inst_len = 0, i;
        qemu_put_be32(f, state_transfer_type); // send state_transfer_type
        qemu_put_be32(f, chkpt_mserv_cnt); 
        for(i = 0; i < chkpt_mserv_cnt; i++){
            inst_len = strlen(chkpt_recovery_inst_str[i]);  
            qemu_put_be32(f, inst_len); 
            qemu_put_buffer(f, (uint8_t *) chkpt_recovery_inst_str[i], inst_len);
printf("hash_save_block: statetype = %d mserv_cnt = %d inst_len = %d reocvery_str [%d] = %s \n", 
state_transfer_type, chkpt_mserv_cnt, inst_len, i, chkpt_recovery_inst_str[i]);
fflush(stdout);
        }
    }
    else{
      printf("ram_load: unknown state transfer type  = %d \n", state_transfer_type);
      exit(-1);
    }

/*
    for(tmp_ptr = ram_cow_buffer; tmp_ptr != NULL; tmp_ptr =(ram_cow_t *)(tmp_ptr->hh.next)){
	
	current_addr = tmp_ptr->ram_addr;
	
	if(current_addr & HASH_EXCLUSION_FLAG){
		if(tmp_ptr->storage_type == LOCAL_MEM_STORAGE){
			num_skip_hash_block++;
			num_skip_local_mem++;
			// Assertion
			if(tmp_ptr->contents != NULL){
				printf("hash_save_block: NULL contents expected!\n");
				fflush(stdout);	
			}
			//qemu_put_be64(f, current_addr | RAM_SAVE_FLAG_FILE_STORAGE);
		}
		else{
	    		printf("Exclusion: Unknown storage type %d \n",tmp_ptr->storage_type);
		}
		continue;	           
	}
	saved_blocks++;
	
	if(tmp_ptr->storage_type == LOCAL_MEM_STORAGE){
	    //qemu_put_be64(f, current_addr | RAM_SAVE_FLAG_FILE_STORAGE);
	    	saved_blocks_local_mem++;
		p = tmp_ptr->contents;

          	if (is_dup_page(p, *p)) {
                	qemu_put_be64(f, current_addr | RAM_SAVE_FLAG_COMPRESS);
                	qemu_put_byte(f, *p);
			bytes_transferred += 1;
          	} else {
                	qemu_put_be64(f, current_addr | RAM_SAVE_FLAG_PAGE);
                	qemu_put_buffer(f, p, TARGET_PAGE_SIZE);
			bytes_transferred += TARGET_PAGE_SIZE;
			pages_transferred++;
          	}
	}
	else{
	    printf("Unknown storage type %d \n",tmp_ptr->storage_type);
	}	           
    } 
*/
// End section 2 here
    qemu_put_be64(f, RAM_SAVE_FLAG_EOS);    

DREG{printf("Stage 4 pages saved in hash table = %d \n", saved_blocks);
fflush(stdout);}
    
// free the hash structure
    num_hash_freed = 0;
    
    while(ram_cow_buffer){
	tmp_ptr = ram_cow_buffer;
	ram_cow_buffer =(ram_cow_t *)(tmp_ptr->hh.next); 
	
	current_addr = tmp_ptr->ram_addr;
	
	free((void *)tmp_ptr);	
	num_hash_freed++;
    }
DDETAIL{printf("hash_save_block: num_hash_freed=%d\n", num_hash_freed);
fflush(stdout);}
                    
// TLC save paralllel buffer to file
    qemu_put_byte(f, QEMU_VM_SECTION_END);
    qemu_put_be32(f, tlc_ram_section_id);

//printf(" hsb: tlc_vm_hash_table = %" PRId64 "\n", tlc_vm_hash_table_add);

    if(tlc_vm_hash_table_add){
    
    for(j = 0; j < num_cp_threads; j++){
        int buffer_index = (ram_contents+j)->start;
	int num_pages = (ram_contents+j)->num;
DREG{printf("hsb(j=%d): buffer_index=%d\n", j, buffer_index);
printf("hsb(j=%d): num_pages=%d\n", j, num_pages);
fflush(stdout);}

	int i = 0;
		
        while(i < num_pages){
	  current_addr = (addr_buffer+buffer_index)->addr;
	  p = (addr_buffer+buffer_index)->mem_contents;
	  if (p){
	    saved_buffer_to_chkpt_file++;
            if (is_dup_page(p, *p)) {
                qemu_put_be64(f, current_addr | RAM_SAVE_FLAG_COMPRESS);
                qemu_put_byte(f, *p);
		bytes_transferred += 1;
            } else {
                qemu_put_be64(f, current_addr | RAM_SAVE_FLAG_PAGE);
                qemu_put_buffer(f, p, TARGET_PAGE_SIZE);
		bytes_transferred += TARGET_PAGE_SIZE;
		pages_transferred++;
            }
	  }            
// TLC: count number of saved bytes and pages		
	  buffer_index++;
	  i++;
        }		
DREG{printf("Stage 4 RamNode = %d Partial pages sent = %d pages\n", j, i);
fflush(stdout);}    
    }
    
    } // num vm add not zero
    qemu_put_be64(f, RAM_SAVE_FLAG_EOS);

DREG{printf("Stage 4 Total page sent =%d\n", 
	saved_buffer_to_chkpt_file);
}   

    return 0;
}

extern int idone;
extern ram_addr_t	vm_last_ram_offset;

extern pthread_rwlock_t dirty_page_rwlock; 
// TLC s2.2 remote vs local page saving stat
static uint64_t num_ps_store_remote = 0;
static uint64_t num_ps_store_local = 0;

int ram_save_to_hash_worker(void)
{ 
    static ram_addr_t 	current_addr = 0; // keep the current addr
    ram_addr_t 		saved_addr = current_addr;
    ram_addr_t 		addr = 0;
    int 		found = 0;
// TLC: this function saves a memory block to hash 
// It return with 1 if found. Otherwise, it return 0. 
// This funtion would be called repeatedly. 
//if(idone){
//printf(" in rsthw\n");
//fflush(stdout);
//}
    while (addr < vm_last_ram_offset) {
    
        pthread_rwlock_rdlock(&dirty_page_rwlock);
	if (tlc_cpu_physical_memory_get_dirty(current_addr, MIGRATION_DIRTY_FLAG)
	   && !tlc_cpu_physical_memory_get_dirty(current_addr, IO_DIRTY_FLAG)){
            uint8_t *p;
	    
	    pthread_rwlock_unlock(&dirty_page_rwlock);
	    // We use IO_DIRTY_FLAG to make sure that any dirty bit updates (set)
	    // by any other parts of codes not through cpu_physical_XX interfaces
	    // get transfer at Stage 3.
	    // Here, pages that have both MIGRATION_DIRTY_FLAG and 
	    // IO_DIRTY_FLAG set are not copied and reset dirty bit values.

            p = tlc_qemu_get_ram_ptr(current_addr);

	    if(put_page_to_hash(current_addr, p) == 1){
	    	num_ps_store_remote++;
	    }
	    else{
                found = -1;
                break;
	      	//num_ps_store_local++;
	    }
	    pthread_rwlock_rdlock(&dirty_page_rwlock);
            //cpu_physical_memory_reset_dirty(current_addr,
            //             current_addr + TARGET_PAGE_SIZE,
            //             (NEW_UPDATED_DIRTY_FLAG | MIGRATION_DIRTY_FLAG));
            tlc_cpu_physical_memory_reset_dirty_flags(current_addr,
                         (NEW_UPDATED_DIRTY_FLAG | MIGRATION_DIRTY_FLAG));
            //cpu_physical_memory_reset_dirty_flags(current_addr,
            //             (NEW_UPDATED_DIRTY_FLAG | MIGRATION_DIRTY_FLAG));
	    pthread_rwlock_unlock(&dirty_page_rwlock);
            found = 1;
            break;
        }
	else{
	    pthread_rwlock_unlock(&dirty_page_rwlock);
	}
        addr += TARGET_PAGE_SIZE;
        current_addr = (saved_addr + addr) % vm_last_ram_offset;
    }  
//if(idone){
//printf(" out rsthw\n");
//fflush(stdout);
//}
    return found;
}

extern pthread_mutex_t 	mutex_idone;
extern pthread_cond_t  save_dev_state_done;
extern int save_dev_state; 
extern void mc_close(void);
extern void mc_close2(void);
extern void mserv_close(void);

int *tlc_s3_pages_sent_mserv = NULL; // count number of pages each thr sent in TLM
void transfer_ram_content_to_dst(void);

static void tlc_close_s2_connections(void){
    if(
       (state_transfer_type == TLM)||
       (state_transfer_type == TLM_STANDBY_DST)||
       (state_transfer_type == TLM_STANDBY_SRCDST)||
       (state_transfer_type == TLMC)||
       (state_transfer_type == TLMC_STANDBY_DST)
      ){
        mc_close();
    }
    else if(
       (state_transfer_type == TLC_EXEC)||
       (state_transfer_type == TLC_TCP_STANDBY_DST)
      ){
        printf("close s2 conn: noop\n");
    }
    else{
        printf("close s2 conn: unknown state transfer type  = %d \n", state_transfer_type);
    }
}

static void tlc_close_s3_connections(void){
    if(
       (state_transfer_type == TLM)||
       (state_transfer_type == TLM_STANDBY_DST)||
       (state_transfer_type == TLM_STANDBY_SRCDST)||
       (state_transfer_type == TLMC)||
       (state_transfer_type == TLMC_STANDBY_DST)
      ){
        mc_close2();
        //mserv_close(); // temp
    }
    else if(
       (state_transfer_type == TLC_EXEC)||
       (state_transfer_type == TLC_TCP_STANDBY_DST)
      ){
        printf("close s2 conn: mserv_close()\n");
        mserv_close();
    }
    else{
        printf("close s2 conn: unknown state transfer type  = %d \n", state_transfer_type);
    }
}

void *page_saver_t(void *args){
    int save_status = 0;

    while(1){
        //if(idone){
	//printf("Page saver before lock1 nodirty = %d stage2_done = %d\n", nodirty, stage2_done);
        //fflush(stdout); 
        //}
       
        pthread_mutex_lock(&mutex_save_page);
	while(nodirty){
//if(idone){
//printf(" PS: in wait loop\n");
//fflush(stdout);
//}
		pthread_cond_wait(&have_dirty, &mutex_save_page);
	}
//if(idone){
//printf(" PS: out wait loop\n");
//fflush(stdout);
//}

	if(idone){
	    // This flag will be set (by buffere_rate_tick_m())
	    // when idone is set and nodirty is true.

            tlc_close_s2_connections();
	    stage2_done = 1;

            pthread_mutex_lock(&mutex_theone);

            pthread_mutex_unlock(&mutex_save_page);

            pthread_mutex_lock(&mutex_theone);
            pthread_mutex_unlock(&mutex_theone);

            if((state_transfer_type == TLC_EXEC)||
               (state_transfer_type == TLC_TCP_STANDBY_DST)||
               (state_transfer_type == TLMC_STANDBY_DST)||
               (state_transfer_type == TLMC)
              ){
                transfer_ram_content_to_dst();
            }

            tlc_close_s3_connections();

	    pthread_mutex_lock(&mutex_idone);
	    save_dev_state = 1;	
	    pthread_cond_signal(&save_dev_state_done);	
	    pthread_mutex_unlock(&mutex_idone);

	    break;
	}
	else{

            if (s22_mig_trans_failure == 0){ // if fail during stage 2, wait for 2.1 to bail 
	        save_status = ram_save_to_hash_worker();
            
	        if (save_status == 0){
	            nodirty = 1;
	        }
	        else if(save_status == 1){
	            pages_copied_last_epoch++; // count number of pages copied to hash so far 
	        }
            }

            pthread_mutex_unlock(&mutex_save_page); // must not overlap below with cpu sync

        }
    } 

    if(s3_mig_trans_failure){
        printf("page_saver_t: Stage 4 error s3_failure is set\n"); 
    }
    else{
        DREG{
        printf("PS: number of pages added to hash table (cannot send out) = %" PRId64 "\n", tlc_vm_hash_table_add);
        printf("PS: num_store_remote=%" PRId64 " | num_store_local= %" PRId64 "\n", 
    	    num_ps_store_remote, num_ps_store_local);
        printf("PS: thr terminates\n");
        fflush(stdout);}  
    }
    
    pthread_exit((void *)0);
}

int tlc_handle_dirty_ram(void){
	int r;
	
        if ((r = cpu_physical_sync_dirty_bitmap(0, TARGET_PHYS_ADDR_MAX)) != 0) {
	    //pthread_rwlock_unlock(&dirty_page_rwlock);
            //qemu_file_set_error(f);
	    DREG{printf("Error! cannot sync dirty bits\n");
	    fflush(stdout);}
            return 0;
        }
	 	
	return r;
}

// Parallle saving (Stage 3) related functions below

void addr_buffer_init(void){
    // initialize addr_buffer memory 
    addr_buffer = (addr_buffer_t *)malloc(ADDR_BUFFER_CHUNK*sizeof(addr_buffer_t));
    addr_buffer_size = ADDR_BUFFER_CHUNK;
    last_addr_buffer = 0; 
    
    DREG{printf("addr_buffer_init: addr_buf_size=%lu, last_addr_buf=%lu\n",
	addr_buffer_size, last_addr_buffer);
	fflush(stdout);}
}

addr_buffer_t * add_addr_buffer(ram_addr_t addr, uint8_t *p){
    addr_buffer_t *new_addr_item;
    
    if(last_addr_buffer == addr_buffer_size){
    	addr_buffer = (addr_buffer_t *)realloc(
		(void *)addr_buffer, 
		(addr_buffer_size + ADDR_BUFFER_CHUNK)*
    		sizeof(addr_buffer_t));
   	addr_buffer_size += ADDR_BUFFER_CHUNK;    

    }
    new_addr_item = (addr_buffer+last_addr_buffer);
    new_addr_item->addr = addr;
    new_addr_item->ptr = p;
    new_addr_item->mem_contents = NULL;
    
    last_addr_buffer++;
    
    return new_addr_item;
}


void *copier_t (void *t){

    int k, ret; 
    
    uint64_t mkey; 
    size_t   key_length = sizeof(uint64_t); 
    size_t   value_length = sizeof(uint8_t)*TARGET_PAGE_SIZE;
    uint8_t *saved_ptr;
    
    ram_content_t *my_ram_contents;
    addr_buffer_t *current_addr_buffer;
    int my_addr_index;
    int my_addr_num;
    uint8_t *my_memory_buffer = NULL;
    uint8_t *my_addr_contents = NULL;

    long my_cp_tid = (long)t; 
    trans_err_flags[my_cp_tid] = tlc_s3_pages_sent_mserv[my_cp_tid] = 0;  

    my_ram_contents = ram_contents + my_cp_tid;
        
    my_addr_index = my_ram_contents->start;
    my_addr_num = my_ram_contents->num;
    if((state_transfer_type == TLC_EXEC)||
       (state_transfer_type == TLC_TCP_STANDBY_DST)||
       (state_transfer_type == TLMC_STANDBY_DST)||
       (state_transfer_type == TLMC)
      ){
        my_memory_buffer = my_ram_contents->memory_buffer;
    }
    
    DREG{printf("copier_t: my_addr_index(start)=%d, num=%d\n", 
    	my_addr_index, my_addr_num);
    fflush(stdout);}
	    
    if((state_transfer_type == TLC_EXEC)||
       (state_transfer_type == TLC_TCP_STANDBY_DST)||
       (state_transfer_type == TLMC_STANDBY_DST)||
       (state_transfer_type == TLMC)
      ){
        for(k = 0; k < my_addr_num; k ++){
            current_addr_buffer = addr_buffer+my_addr_index;
	    saved_ptr = current_addr_buffer->ptr;
	    //my_addr_contents = current_addr_buffer->mem_contents;	
	    mkey = current_addr_buffer->addr;	

	    memcpy(my_memory_buffer, saved_ptr, value_length);

	    current_addr_buffer->mem_contents = my_memory_buffer;	
            my_memory_buffer += value_length;

	    my_addr_index++;
        }
    }
    else if(
            (state_transfer_type == TLM)||
            (state_transfer_type == TLM_STANDBY_DST)||
            (state_transfer_type == TLM_STANDBY_SRCDST)
        ){ // TLM
        for(k = 0; k < my_addr_num; k ++){
            current_addr_buffer = addr_buffer+my_addr_index;
	    saved_ptr = current_addr_buffer->ptr;
	    my_addr_contents = current_addr_buffer->mem_contents;	
	    mkey = current_addr_buffer->addr;	

	    // send page to membase servers
	    ret= mc_set2((const char *) &mkey, key_length, (const char *) saved_ptr, value_length);
	
	    if (ret < 0){
		printf("copier_t: Couldn't store key:ret = %d \n", ret);
                /* 
		if(my_addr_contents == NULL){
	  		my_addr_contents = current_addr_buffer->mem_contents =
	  		(uint8_t *)malloc(sizeof(uint8_t)*TARGET_PAGE_SIZE);
		}
		memcpy(my_addr_contents, saved_ptr, TARGET_PAGE_SIZE); 	
		tlc_s3_pages_sent_local++;			
                */
                trans_err_flags[my_cp_tid] = 1;
                goto out_copy_t; // end this thread 
	    }
	    else{
		// remove later
		if(my_addr_contents != NULL){
	        	free(my_addr_contents);
			current_addr_buffer->mem_contents = NULL;
		}
		tlc_s3_pages_sent_mserv[my_cp_tid]++;    		
	    }	
	    my_addr_index++;
        }
    }
    else{
        trans_err_flags[my_cp_tid] = 1;
    }
    
    DREG{printf("copier_t: my_addr_index(end)=%d, cp_to_mserv=%d\n", 
    my_addr_index, tlc_s3_pages_sent_mserv[my_cp_tid]);
    fflush(stdout);}

out_copy_t: 
    pthread_exit((void *)t);

}

//#define COPYING_ADDRESSES_PER_THREAD	32768

extern int ncpus;	

int get_num_cp_threads(void){
    int ret = 1;
/*        
    if(last_addr_buffer < COPYING_ADDRESSES_PER_THREAD){
    	ret = 1;
    }
    else if(last_addr_buffer >= COPYING_ADDRESSES_PER_THREAD * ncpus){
    	ret = ncpus;
    }
    else{
    	ret = (last_addr_buffer/COPYING_ADDRESSES_PER_THREAD)+1;
    }
*/
//    ret = ncpus;
    DREG{printf("get_num_cp_thr: last_addr_buf=%ld, ret(thr num)=%d\n", 
    	last_addr_buffer, ret);
    fflush(stdout);}

    return(ret);
}

void copying_pages_in_addr_buffer(void){
    int j, k, ret; 
    
    uint64_t mkey; 
    size_t   key_length = sizeof(uint64_t); 
    size_t   value_length = sizeof(uint8_t)*TARGET_PAGE_SIZE;
   
    uint8_t *saved_ptr;
    int tid;
    pthread_t *thr = NULL;
    pthread_attr_t attr;
    void *status;
    
    ram_content_t *my_ram_contents;
    addr_buffer_t *current_addr_buffer;
    int my_addr_index;
    int my_addr_num;
    uint8_t *my_memory_buffer = NULL;
    uint8_t *my_addr_contents = NULL;

    long my_cp_tid = 0;
    
    // VIC stuffs
    int vic_local_counter = 0;
    int vic_one_tenth = 0;
    int vic_accumulate = 0;
    char progress_string[VIC_STR_LEN];

    num_cp_threads = get_num_cp_threads(); // at least 1
    
    ram_contents = (ram_content_t *)malloc(num_cp_threads *
    			sizeof(ram_content_t));
    // global vars to detect transmisson errors
    trans_err_flags = (int *)malloc(num_cp_threads * sizeof(int)); 
    tlc_s3_pages_sent_mserv = (int *)malloc(num_cp_threads * sizeof(int)); 
    
    for(j = 0; j < num_cp_threads; j++){
    	(ram_contents+j)->start = j*((int)(last_addr_buffer/num_cp_threads));
        if(j == num_cp_threads - 1) 
          (ram_contents+j)->num = last_addr_buffer - (ram_contents+j)->start;
        else
	  (ram_contents+j)->num = (int)(last_addr_buffer/num_cp_threads);

        // allocate memory_buffer for TLC and TLMC
        if((state_transfer_type == TLC_EXEC)||
           (state_transfer_type == TLC_TCP_STANDBY_DST)||
           (state_transfer_type == TLMC_STANDBY_DST)||
           (state_transfer_type == TLMC)
          ){
            if(((ram_contents+j)->memory_buffer = (uint8_t *)malloc(TARGET_PAGE_SIZE * 
                                                            ((ram_contents+j)->num))) == NULL){;  
                trans_err_flags[0] = 1;
                goto out_copy_buffer;
            }
        }
        else{
            (ram_contents+j)->memory_buffer = NULL; 
        }

	DREG{printf("cp_buffer: ram_con j=%d, start=%d, num=%d\n", j,(ram_contents+j)->start,
		 (ram_contents+j)->num);
	fflush(stdout);}
    }
    
    DREG{printf("cp_buffer: last_addr_buffer=%lu\n", last_addr_buffer);
    fflush(stdout);}    

    DVIC{
       if(vic_flag){
           sprintf(progress_string, "s3 %" PRId64 " %d", last_addr_buffer, num_cp_threads);
	   vic_report(progress_string);
       }
       //printf("NumS3pages %" PRId64 ": NumCPthr %d \n", last_addr_buffer, num_cp_threads);
       printf("s3 %" PRId64 " %d \n", last_addr_buffer, num_cp_threads);
    }

    if (num_cp_threads > 1){   
        thr = (pthread_t *)malloc(num_cp_threads * sizeof(pthread_t));
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE); 
	 
	for(tid = 1; tid <= (num_cp_threads - 1); tid++){
	  // create a worker thread ;
	  pthread_create((thr+tid), &attr, copier_t, (void *)((long)tid));
	  //pthread_detach(thr);
	}
    }
    // copy thread computation part   
    my_cp_tid = 0; 
    trans_err_flags[my_cp_tid] = tlc_s3_pages_sent_mserv[my_cp_tid] = 0;  
    
    my_ram_contents = ram_contents + my_cp_tid;
        
    my_addr_index = my_ram_contents->start;
    my_addr_num = my_ram_contents->num;
    if((state_transfer_type == TLC_EXEC)||
       (state_transfer_type == TLC_TCP_STANDBY_DST)||
       (state_transfer_type == TLMC_STANDBY_DST)||
       (state_transfer_type == TLMC)
      ){
        my_memory_buffer = my_ram_contents->memory_buffer;
    }
    
    DREG{printf("cp_buffer: cptid=%ld, my_addr_index(start)=%d, num=%d\n", 
    	my_cp_tid, my_addr_index, my_addr_num);
    fflush(stdout);}

    // VIC stuffs
    if(vic_flag){
        vic_local_counter = 0;
        vic_one_tenth = (int)(my_addr_num/10);
	vic_accumulate = 0;
    }

    if((state_transfer_type == TLC_EXEC)||
       (state_transfer_type == TLC_TCP_STANDBY_DST)||
       (state_transfer_type == TLMC_STANDBY_DST)||
       (state_transfer_type == TLMC)
      ){
        for(k = 0; k < my_addr_num; k++){
            current_addr_buffer = addr_buffer+my_addr_index; 
	    saved_ptr = current_addr_buffer->ptr;
	    //my_addr_contents = current_addr_buffer->mem_contents;	
	    mkey = current_addr_buffer->addr;	
        
	    memcpy(my_memory_buffer, saved_ptr, value_length);

	    current_addr_buffer->mem_contents = my_memory_buffer;	
            my_memory_buffer += value_length;

	    // VIC stuffs
	    if(vic_flag){
		vic_local_counter++;
		    if(vic_local_counter >= vic_one_tenth){
			char progress_string[VIC_STR_LEN];

			vic_accumulate += 10;
			vic_local_counter = 0;  
	    		snprintf(progress_string, VIC_STR_LEN,"st %d", vic_accumulate);
	    		vic_report(progress_string);
		    }
	    }	
	    my_addr_index++;
        }
    }
    else if( 
            (state_transfer_type == TLM)||
            (state_transfer_type == TLM_STANDBY_DST)||
            (state_transfer_type == TLM_STANDBY_SRCDST)
        ){ // TLM
        for(k = 0; k < my_addr_num; k++){
            current_addr_buffer = addr_buffer+my_addr_index; 
	    saved_ptr = current_addr_buffer->ptr;
	    my_addr_contents = current_addr_buffer->mem_contents;	
	    mkey = current_addr_buffer->addr;	
        
	    // send page to mem servers
	    ret= mc_set2((const char *) &mkey, key_length, (const char *) saved_ptr, value_length);
	
	    if (ret < 0){
		printf("cp_buffer: Couldn't store key:ret = %d \n", ret);
                /* 
                // The code below write data to memory if network writes fal.
                // We skip this for now.
		if(my_addr_contents == NULL){
	  		my_addr_contents = current_addr_buffer->mem_contents =
	  		(uint8_t *)malloc(sizeof(uint8_t)*TARGET_PAGE_SIZE);
		}
		memcpy(my_addr_contents, saved_ptr, TARGET_PAGE_SIZE); 	
		tlc_s3_pages_sent_local++;			
                */
                // migration fails
                trans_err_flags[my_cp_tid] = 1;
                goto out_copy_buffer;
	    }
	    else{
		// remove later
		if(my_addr_contents != NULL){
			free(my_addr_contents);
			current_addr_buffer->mem_contents = NULL;
		}
		tlc_s3_pages_sent_mserv[my_cp_tid]++;    		

		// VIC stuffs
		if(vic_flag){
			vic_local_counter++;
			if(vic_local_counter >= vic_one_tenth){
				char progress_string[VIC_STR_LEN];

				vic_accumulate += 10;
				vic_local_counter = 0;
	    			snprintf(progress_string, VIC_STR_LEN,"st %d", vic_accumulate);
	    			vic_report(progress_string);
			}
		}

	   }	
	   my_addr_index++;
        }
    }
    else{
        trans_err_flags[my_cp_tid] = 1;
    }

out_copy_buffer:
    if (num_cp_threads > 1){
    	pthread_attr_destroy(&attr);
	
	for(tid = 1; tid <= (num_cp_threads - 1); tid++){
	  // wait for the termination of copier_t threads
	  pthread_join(*(thr+tid), &status);
	}
	free(thr);
    }          

    for (k=0; k< num_cp_threads; k++){ // check if trans fails
        if(trans_err_flags[k] == 1){
          s3_mig_trans_failure = 1;
          break;
        }
    }

    if (s3_mig_trans_failure){
        if(vic_flag){ 
          vic_report((char *)"s3 error");
        }
        printf("cp_buffer: s3 transmission failures!\n");
        fflush(stdout);
    }
    else{
        // VIC stuffs
        if(vic_flag){ 
          vic_report((char *)"st 100");
        }
  
        DREG{
          printf("cp_buffer: tid=%ld, my_addr_index(end)=%d, cp_to_mserv=%d\n", 
    	  my_cp_tid, my_addr_index, tlc_s3_pages_sent_mserv[my_cp_tid]);
          fflush(stdout);
        }
    }
}

int ram_save_to_buffer_parallel(void)
{
    ram_addr_t addr = 0;
    ram_cow_t *tmp_hash_ptr;
    addr_buffer_t *new_addr_item;

uint64_t tlc_ram_list_copied = 0;
uint64_t ram_list_copied = 0;

    addr_buffer_init();
    
    while (addr < vm_last_ram_offset) {

        if (tlc_lock_free_cpu_physical_memory_get_dirty(addr, MIGRATION_DIRTY_FLAG)) {
            uint8_t *p;
	    
	    p = tlc_qemu_get_ram_ptr(addr);  

	    new_addr_item = add_addr_buffer(addr, p);
	    if(tlc_vm_hash_table_add){
	        if((tmp_hash_ptr = exclude_page_from_hash(addr))!=NULL){ // exclude hash contents
	            if(tmp_hash_ptr->contents != NULL){
			new_addr_item->mem_contents = tmp_hash_ptr->contents;
			tmp_hash_ptr->contents = NULL;
		    }
	        }
	    }
tlc_ram_list_copied++;
        }
        else if (cpu_physical_memory_get_dirty(addr, MIGRATION_DIRTY_FLAG)) {
            uint8_t *p;
	    
	    p = tlc_qemu_get_ram_ptr(addr);  

	    new_addr_item = add_addr_buffer(addr, p);
	    if(tlc_vm_hash_table_add){
	        if((tmp_hash_ptr = exclude_page_from_hash(addr))!=NULL){ // exclude hash contents
	            if(tmp_hash_ptr->contents != NULL){
			new_addr_item->mem_contents = tmp_hash_ptr->contents;
			tmp_hash_ptr->contents = NULL;
		    }
	        }
	    }
ram_list_copied++;
        }
        addr += TARGET_PAGE_SIZE;
        //current_addr = (saved_addr + addr) % last_ram_offset;
    }
    // copy pages to memory buffer
    copying_pages_in_addr_buffer();

    if(s3_mig_trans_failure == 1){
        return -1;
    }
    else{
        DREG{ 
        printf("tlc_ram_list_copied = %" PRId64 " pages : ram_list_copied = %" PRId64 " pages\n", 
    		tlc_ram_list_copied, ram_list_copied);   
        }
        return last_addr_buffer;
    }
}


void transfer_ram_content_to_dst(void){
    int j, k, ret;
    ram_content_t *my_ram_contents;
    addr_buffer_t *current_addr_buffer;
    int my_addr_index;
    int my_addr_num;
    //uint8_t *my_memory_buffer = NULL;
    uint8_t *my_addr_contents = NULL;
    uint64_t mkey; 
    size_t   key_length = sizeof(uint64_t); 
    size_t   value_length = sizeof(uint8_t)*TARGET_PAGE_SIZE;

    for(j = 0; j < num_cp_threads; j++){

        my_ram_contents = ram_contents + j;
        
        my_addr_index = my_ram_contents->start;
        my_addr_num = my_ram_contents->num;
        //my_memory_buffer = my_ram_contents->memory_buffer;

        for(k = 0; k < my_addr_num; k++){
            current_addr_buffer = addr_buffer+my_addr_index; 
	    my_addr_contents = current_addr_buffer->mem_contents;	
	    mkey = current_addr_buffer->addr;	
        
            if(
               (state_transfer_type == TLM)||
               (state_transfer_type == TLM_STANDBY_DST)||
               (state_transfer_type == TLM_STANDBY_SRCDST)||
               (state_transfer_type == TLMC)||
               (state_transfer_type == TLMC_STANDBY_DST)
              ){
	         // send page to mem servers
	         ret = mc_set2((const char *) &mkey, key_length, (const char *) my_addr_contents, value_length);
	         //ret= mserv_set((const char *) &mkey, key_length, (const char *) my_addr_contents, value_length);
            }
            else if(
               (state_transfer_type == TLC_EXEC)||
               (state_transfer_type == TLC_TCP_STANDBY_DST)
              ){
	         ret= mserv_set((const char *) &mkey, key_length, (const char *) my_addr_contents, value_length);
            }
            else{
	         printf("transfer_contents: unknown state transfer type  = %d \n", state_transfer_type);
                 s3_mig_trans_failure = 1;
                 goto out_trans_buffer;
            }
	
	    if (ret < 0){
		printf("transfer_contents: Couldn't transfer key:ret = %d \n", ret);
                /* 
                // The code below write data to memory if network writes fal.
                // We skip this for now.
		if(my_addr_contents == NULL){
	  		my_addr_contents = current_addr_buffer->mem_contents =
	  		(uint8_t *)malloc(sizeof(uint8_t)*TARGET_PAGE_SIZE);
		}
		memcpy(my_addr_contents, saved_ptr, TARGET_PAGE_SIZE); 	
		tlc_s3_pages_sent_local++;			
                */
                // migration fails
                s3_mig_trans_failure = 1;
                goto out_trans_buffer;
	    }
	    else{
		tlc_s3_pages_sent_mserv[j]++;    		

	   }	
	   my_addr_index++;
        }

	DREG{printf("trans_ram_contents: ram_con j=%d, start=%d, num=%d, sent_to_mserv=%d\n", 
                j,(ram_contents+j)->start, (ram_contents+j)->num, tlc_s3_pages_sent_mserv[j]);
	fflush(stdout);}

        if(vic_flag){
	   char progress_string[VIC_STR_LEN];
           snprintf(progress_string, VIC_STR_LEN, "ht %d %d %d\n", j, (ram_contents+j)->num, tlc_s3_pages_sent_mserv[j]);
	   vic_report(progress_string);

        }
        printf("ht %d %d %d\n", j, (ram_contents+j)->num, tlc_s3_pages_sent_mserv[j]);
        fflush(stdout);
    }
out_trans_buffer:
    return;
}

