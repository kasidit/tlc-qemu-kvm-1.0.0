/*
 * Thread-based Live Migration (TLM) and Thread-based Live Checkpointing (TLC). 
 *
 * Copyright Kasidit Chanchio, Vasabilab. 2012
 *
 * Authors:
 *  Kasidit Chanchio   <kasiditchanchio@gmail.com>
 *
 *
 */
extern RAMList tlc_ram_list; // TLC ram

// TLC extension 
#define NEW_UPDATED_DIRTY_FLAG	0x10
#define IO_DIRTY_FLAG		0x20

#include <pthread.h>

extern int mthread;
extern uint64_t tlc_dirty_size;
extern pthread_mutex_t *lock_phys_ram_dirty;  

#define TLC_LOCK_DIRTYBIT_ACCESS(addr) \
	pthread_mutex_lock(&lock_phys_ram_dirty[(addr >> TARGET_PAGE_BITS)% tlc_dirty_size])
#define TLC_UNLOCK_DIRTYBIT_ACCESS(addr) \
	pthread_mutex_unlock(&lock_phys_ram_dirty[(addr >> TARGET_PAGE_BITS) % tlc_dirty_size])
#define TLC_LOCK_DIRTYBIT_BYPAGE(page_id) \
	pthread_mutex_lock(&lock_phys_ram_dirty[(page_id)% tlc_dirty_size])
#define TLC_UNLOCK_DIRTYBIT_BYPAGE(page_id) \
	pthread_mutex_unlock(&lock_phys_ram_dirty[(page_id) % tlc_dirty_size])
	
// TLC ram
static inline int tlc_cpu_physical_memory_get_dirty(ram_addr_t addr,
                                                int dirty_flags)
{
    int ret; 
    TLC_LOCK_DIRTYBIT_ACCESS(addr);
    ret = tlc_ram_list.phys_dirty[addr >> TARGET_PAGE_BITS] & dirty_flags;
    TLC_UNLOCK_DIRTYBIT_ACCESS(addr);
    return ret;
}

static inline int tlc_lock_free_cpu_physical_memory_get_dirty(ram_addr_t addr,
                                                int dirty_flags)
{
    return tlc_ram_list.phys_dirty[addr >> TARGET_PAGE_BITS] & dirty_flags;
}

// TLC
// We create this function to work with kvm_dirty_bit_sync() under rwlock.
// No mutex is needed. We reset the NEW_UPDATED pages here.
static inline void tlc_cpu_physical_memory_set_dirty(ram_addr_t addr)
{ 
    if(unlikely(mthread)){
        tlc_ram_list.phys_dirty[addr >> TARGET_PAGE_BITS] = 0xff & (~IO_DIRTY_FLAG) & (~NEW_UPDATED_DIRTY_FLAG);
        ram_list.phys_dirty[addr >> TARGET_PAGE_BITS] = 0xff & (~MIGRATION_DIRTY_FLAG) & (~IO_DIRTY_FLAG) & (~NEW_UPDATED_DIRTY_FLAG);
    }
    else{
	// Need this when dirty bit sync is called by non-migration operations
        ram_list.phys_dirty[addr >> TARGET_PAGE_BITS] = 0xff & (~IO_DIRTY_FLAG) & (~NEW_UPDATED_DIRTY_FLAG);
    }
}

static inline void tlc_cpu_physical_memory_set_all_dirty_flags(ram_addr_t addr, int dirty_flags)
{ 
    tlc_ram_list.phys_dirty[addr >> TARGET_PAGE_BITS] = dirty_flags;
    ram_list.phys_dirty[addr >> TARGET_PAGE_BITS] = dirty_flags;
}

// TLC ram
static inline int tlc_cpu_physical_memory_reset_dirty_flags(ram_addr_t addr,
                                                      int dirty_flags)
{
    int ret; 
    TLC_LOCK_DIRTYBIT_ACCESS(addr);
    ret = tlc_ram_list.phys_dirty[addr >> TARGET_PAGE_BITS] &= 
					(~dirty_flags & (~IO_DIRTY_FLAG));
    TLC_UNLOCK_DIRTYBIT_ACCESS(addr);
    return ret; 
}

