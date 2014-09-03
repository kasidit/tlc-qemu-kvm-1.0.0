/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <pthread.h> // TLC 
 
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#ifndef _WIN32
#include <sys/types.h>
#include <sys/mman.h>
#endif
#include "config.h"
#include "monitor.h"
#include "sysemu.h"
#include "arch_init.h"
#include "audio/audio.h"
#include "hw/pc.h"
#include "hw/pci.h"
#include "hw/audiodev.h"
#include "kvm.h"
#include "migration.h"
#include "net.h"
#include "gdbstub.h"
#include "hw/smbios.h"

#ifdef TARGET_SPARC
int graphic_width = 1024;
int graphic_height = 768;
int graphic_depth = 8;
#else
int graphic_width = 800;
int graphic_height = 600;
int graphic_depth = 15;
#endif

const char arch_config_name[] = CONFIG_QEMU_CONFDIR "/target-" TARGET_ARCH ".conf";

#if defined(TARGET_ALPHA)
#define QEMU_ARCH QEMU_ARCH_ALPHA
#elif defined(TARGET_ARM)
#define QEMU_ARCH QEMU_ARCH_ARM
#elif defined(TARGET_CRIS)
#define QEMU_ARCH QEMU_ARCH_CRIS
#elif defined(TARGET_I386)
#define QEMU_ARCH QEMU_ARCH_I386
#elif defined(TARGET_M68K)
#define QEMU_ARCH QEMU_ARCH_M68K
#elif defined(TARGET_LM32)
#define QEMU_ARCH QEMU_ARCH_LM32
#elif defined(TARGET_MICROBLAZE)
#define QEMU_ARCH QEMU_ARCH_MICROBLAZE
#elif defined(TARGET_MIPS)
#define QEMU_ARCH QEMU_ARCH_MIPS
#elif defined(TARGET_PPC)
#define QEMU_ARCH QEMU_ARCH_PPC
#elif defined(TARGET_S390X)
#define QEMU_ARCH QEMU_ARCH_S390X
#elif defined(TARGET_SH4)
#define QEMU_ARCH QEMU_ARCH_SH4
#elif defined(TARGET_SPARC)
#define QEMU_ARCH QEMU_ARCH_SPARC
#elif defined(TARGET_XTENSA)
#define QEMU_ARCH QEMU_ARCH_XTENSA
#endif

const uint32_t arch_type = QEMU_ARCH;

/***********************************************************/
/* ram save/restore */

#define RAM_SAVE_FLAG_FULL     0x01 /* Obsolete, not used anymore */
#define RAM_SAVE_FLAG_COMPRESS 0x02
#define RAM_SAVE_FLAG_MEM_SIZE 0x04
#define RAM_SAVE_FLAG_PAGE     0x08
#define RAM_SAVE_FLAG_EOS      0x10
#define RAM_SAVE_FLAG_CONTINUE 0x20

// TLC
#define RAM_SAVE_FLAG_SYNC_STATE      0x40

// TLC
/*
// TLC: begin extension
#include <pthread.h> // TLC
#include "tlc.h"
#include "tlc-debug.h"

#define mbarrier() __asm__ __volatile__("": : :"memory")

#define		TLC_NUM_MEMLOCK_PER_1GB		4096
#define 	ONEGIGABYTES			(1024 * 1024 * 1024)

uint64_t 	tlc_ram_size = 0;
uint64_t 	tlc_page_size = 0;
uint64_t 	tlc_dirty_size = 0;
ram_addr_t	vm_last_ram_offset;

void 		tlc_init_vars(void);
void 		tlc_init(void);

uint64_t 	tlc_s21_bytes_transferred = 0;
uint64_t        tlc_s21_pages_transferred = 0;
extern int 	ram_save_to_buffer_parallel(void);

pthread_mutex_t *lock_phys_ram_dirty; // TLC dirty page logging variable
//pthread_mutex_t acquire_ram_blocks; // TLC
pthread_mutex_t acquire_tlc_ram_blocks; // TLC ram

// TLC incoming
//extern pthread_mutex_t	mutex_incoming; // TLC incoming migration
//pthread_cond_t  receiving_parallel_pages_done; 
//int		restored_incoming_new_dirty_pages; 

// TLC incoming barrier
#define TLC_INCOMING_BARR_THREADS	2
pthread_barrier_t barr;

pthread_mutex_t *lock_array = NULL;
uint64_t	lock_array_size = 0;

#define LOCK_MEM_TABLE(addr) \
	pthread_mutex_lock(&lock_array[(addr >> TARGET_PAGE_BITS)% lock_array_size])
#define UNLOCK_MEM_TABLE(addr) \
	pthread_mutex_unlock(&lock_array[(addr >> TARGET_PAGE_BITS) % lock_array_size])

uint8_t		*priority_array = NULL;
uint64_t	priority_array_size = 0;
#define 	BASE_PRIORITY	0

//extern void mc_close(void);

void tlc_migration_init(int flag);
void tlc_migration_finish(int flag);

#define	TLC_OUTGOING_PAGES	1
#define TLC_INCOMING_PAGES	2

// VIC stuffs
extern int vic_flag;
extern int vic_report(char *message);

// TLC: end

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
*/
extern pthread_mutex_t acquire_tlc_ram_blocks; // TLC ram
extern uint64_t 	tlc_s21_bytes_transferred;

//static RAMBlock *last_block;
//static ram_addr_t last_offset;

/*
// TLC: begin
extern RAMBlock *tlc_block;
extern pthread_rwlock_t dirty_page_rwlock; 

extern ram_addr_t tlc_last_ram_offset(void);
// TLC: end
int num_stage2_skipped_pages = 0;

// TLC variables for cpu slowdown mechanism
uint64_t  stage2_last_addr; 
int 	stage2_breakpoint = 0; 
double 	slowdown_marker;
int 	trigger_slowdown; // flag marking cpu slowdown 
int 	stepwise_slowdown;  
extern  int slowdown_aggressiveness;

extern int s21_mig_trans_failure;

void cpu_slowdown_init(void);

static int tlc_ram_save_block(QEMUFile *f) // TLC
{  
    ram_addr_t current_addr;
    int bytes_sent = 0;

    current_addr = 0;
    num_stage2_skipped_pages = 0;
    
    while (current_addr < vm_last_ram_offset)
    {
        pthread_rwlock_rdlock(&dirty_page_rwlock);
	stage2_last_addr = current_addr;
	
	if (tlc_cpu_physical_memory_get_dirty(current_addr, NEW_UPDATED_DIRTY_FLAG)) {
            uint8_t *p;
	    pthread_rwlock_unlock(&dirty_page_rwlock);

            if (likely(mthread)) mbarrier();

       	    p = tlc_qemu_get_ram_ptr(current_addr);
	                
            if (is_dup_page(p, *p)) {
//printf("rsb: (dub) block: %s :blkoffset: %lu :offset: %lu :blklen: %lu\n", 
//  (uint8_t *)block->idstr, (unsigned long)block->offset, (unsigned long)offset, (unsigned long)block->length);
//fflush(stdout);
//printf(" rsb: put addr=0x%" PRIx64 " compressed\n ", current_addr);
//fflush(stdout);
                qemu_put_be64(f, current_addr | RAM_SAVE_FLAG_COMPRESS);
                qemu_put_byte(f, *p);

		tlc_s21_bytes_transferred += 1;
		bytes_sent += 1;
            } else {
//printf(" rsb: put addr=0x%" PRIx64 " page\n ", current_addr);
//fflush(stdout);
                qemu_put_be64(f, current_addr | RAM_SAVE_FLAG_PAGE);
                qemu_put_buffer(f, p, TARGET_PAGE_SIZE);
//printf("rsb: saving block: %s :blkoffset: %lu :offset: %lu :blklen: %lu\n", 
//  (uint8_t *)block->idstr, (unsigned long)block->offset, (unsigned long)offset, (unsigned long)block->length);
//fflush(stdout);
		tlc_s21_bytes_transferred += TARGET_PAGE_SIZE;
		tlc_s21_pages_transferred++;
		bytes_sent += TARGET_PAGE_SIZE;
            }            
        }
	else{
	    pthread_rwlock_unlock(&dirty_page_rwlock);

	    num_stage2_skipped_pages++;
	}

        if(s21_mig_trans_failure == 1){
            bytes_sent = -1;
            break; 
        }

        current_addr += TARGET_PAGE_SIZE;


    } // while
    return bytes_sent;
}

#define	DEFAULT_SLOWDOWN_MARKER		0.5
#define	DEFAULT_SLOWDOWN_AGGRESSIVE	1

extern int 	cpu_break_switch;

extern char *tlc_slowdown_trigger_str;
extern char *tlc_slowdown_stepwise_str;
extern char *tlc_slowdown_aggressiveness_str;
extern char *tlc_slowdown_marker_str;

void cpu_slowdown_init(void){
    const char *trigger_string; 
    const char *stepwise_string; 
    const char *number_string; 
    const char *marker_string; 
    
    // initialize variables
    stage2_breakpoint = 0;
    cpu_break_switch = 0;
    
    if((trigger_string = tlc_slowdown_trigger_str) == NULL){
    	trigger_slowdown = 0;
    }
    else{
	if(strncmp(trigger_string, "ON", 2) == 0){
    	    trigger_slowdown = 1;
	}
	else{
    	    trigger_slowdown = 0;
	}
    }

    if((stepwise_string = tlc_slowdown_stepwise_str) == NULL){
    	stepwise_slowdown = 0;
    }
    else{
	if(strncmp(stepwise_string, "ON", 2) == 0){
    	    stepwise_slowdown = 1;
	}
	else{
    	    stepwise_slowdown = 0;
	}
    }

    if((marker_string = tlc_slowdown_marker_str) == NULL){
    	slowdown_marker = DEFAULT_SLOWDOWN_MARKER;
    }
    else{
    	slowdown_marker = atof(marker_string);
    }

    if((number_string = tlc_slowdown_aggressiveness_str) == NULL){
        slowdown_aggressiveness = DEFAULT_SLOWDOWN_AGGRESSIVE;
    }
    else{
        slowdown_aggressiveness = atoi(number_string);

	if(slowdown_aggressiveness <= 0){
    	    trigger_slowdown = 0;
	}
    }

printf(" slowdown stepwise = %d trigger = %s marker = %.2lf agress = %d\n", 
stepwise_slowdown, trigger_string, slowdown_marker, slowdown_aggressiveness);
 
}

uint64_t tlc_ram_page_remaining(void);

uint64_t tlc_ram_page_remaining(void)
{
    uint64_t count = 0;

    ram_addr_t addr = 0;
    for (addr = 0; addr < vm_last_ram_offset; addr += TARGET_PAGE_SIZE) {
        if (cpu_physical_memory_get_dirty(addr, MIGRATION_DIRTY_FLAG)) {
            count++;
        }
    }

    return count;
}
*/

static ram_addr_t ram_save_remaining(void)
{
    RAMBlock *block;
    ram_addr_t count = 0;

    QLIST_FOREACH(block, &ram_list.blocks, next) { 
        ram_addr_t addr;
        for (addr = block->offset; addr < block->offset + block->length;
             addr += TARGET_PAGE_SIZE) {
            if (cpu_physical_memory_get_dirty(addr, MIGRATION_DIRTY_FLAG)) {
                count++;
            }
        }
    }

    return count;
}

uint64_t ram_bytes_remaining(void)
{
    return ram_save_remaining() * TARGET_PAGE_SIZE;
}

//uint64_t ram_bytes_transferred(void)
//{
//    return tlc_s21_bytes_transferred; // TLC modified
//}

uint64_t ram_bytes_total(void)
{
    RAMBlock *block;
    uint64_t total = 0;

    QLIST_FOREACH(block, &ram_list.blocks, next) 
        total += block->length;

    return total;
}

/*
static int block_compar(const void *a, const void *b)
{
    RAMBlock * const *ablock = a;
    RAMBlock * const *bblock = b;
    if ((*ablock)->offset < (*bblock)->offset) {
        return -1;
    } else if ((*ablock)->offset > (*bblock)->offset) {
        return 1;
    }
    return 0;
}

static void sort_ram_list(void)
{
    RAMBlock *block, *nblock, **blocks;
    int n;
    n = 0;
    QLIST_FOREACH(block, &ram_list.blocks, next) { 
        ++n;
    }
    
    blocks = g_malloc(n * sizeof *blocks);
    n = 0;
    QLIST_FOREACH_SAFE(block, &ram_list.blocks, next, nblock) { 
        blocks[n++] = block;
        QLIST_REMOVE(block, next);
    }
    
    qsort(blocks, n, sizeof *blocks, block_compar);
    while (--n >= 0) {
        QLIST_INSERT_HEAD(&ram_list.blocks, blocks[n], next); 
    }
    g_free(blocks);
}
*/
/*
// TLC clear all dirty flag values
void tlc_clear_all_dirty_flags(void);

void tlc_clear_all_dirty_flags(void){
    ram_addr_t addr;
    RAMBlock *block;

    QLIST_FOREACH(block, &ram_list.blocks, next) { // TLC modified
        for (addr = block->offset; addr < block->offset + block->length;
            addr += TARGET_PAGE_SIZE) {
                //if (!cpu_physical_memory_get_dirty(addr,
                //                                   MIGRATION_DIRTY_FLAG)) {
                //    cpu_physical_memory_set_dirty(addr);
                //}
		// TLC: initialize new migration and io dirty bits to zero
	    	//cpu_physical_memory_and_dirty(addr, ~MIGRATION_DIRTY_FLAG);
	    	//cpu_physical_memory_and_dirty(addr, ~IO_DIRTY_FLAG);

            tlc_cpu_physical_memory_set_all_dirty_flags(addr, 0xff & ~MIGRATION_DIRTY_FLAG & ~IO_DIRTY_FLAG);
        }
    }

    //cpu_physical_memory_set_dirty_range(0, vm_last_ram_offset, 0xff & 
    //				~MIGRATION_DIRTY_FLAG & ~IO_DIRTY_FLAG);
}

int ram_save_live(Monitor *mon, QEMUFile *f, int stage, void *opaque)
{
    ram_addr_t addr;
    int ret = 0;
    int bytes_sent = 0;
    
    if (stage < 0) {
        cpu_physical_memory_set_dirty_tracking(0);
        return 0;
    }

    if (cpu_physical_sync_dirty_bitmap(0, TARGET_PHYS_ADDR_MAX) != 0) {
        qemu_file_set_error(f, -EINVAL);
        return -EINVAL;
    }

    if (stage == 1) {
        RAMBlock *block;
        last_block = NULL;
        last_offset = 0;
        sort_ram_list();

        cpu_physical_memory_set_dirty_tracking(1);

        qemu_put_be64(f, ram_bytes_total() | RAM_SAVE_FLAG_MEM_SIZE);

        pthread_mutex_lock(&acquire_tlc_ram_blocks); // TLC ram
	QLIST_FOREACH(block, &tlc_ram_list.blocks, next) { // TLC ram
            qemu_put_byte(f, strlen(block->idstr));
            qemu_put_buffer(f, (uint8_t *)block->idstr, strlen(block->idstr));
            qemu_put_be64(f, block->length);
        }
	pthread_mutex_unlock(&acquire_tlc_ram_blocks); // TLC ram
	qemu_put_be64(f, RAM_SAVE_FLAG_EOS);
    }    
    else if(stage == 2){

        bytes_sent = tlc_ram_save_block(f);
        if(bytes_sent < 0){
            ret = bytes_sent;
        }
        tlc_s21_bytes_transferred += bytes_sent;
//printf(" rsl: put RAM_SAVE_FLAG_EOS=%" PRIx64 "\n", RAM_SAVE_FLAG_EOS);
	qemu_put_be64(f, RAM_SAVE_FLAG_EOS);
    
    } // stage 2
    else if (stage == 3) {
	
        bytes_sent = ram_save_to_buffer_parallel();	
        if(bytes_sent < 0){
            ret = bytes_sent;
        }
        tlc_s21_bytes_transferred += bytes_sent;
        
	//printf(" s3: mc_close() is called\n");
	//mc_close(); 
	
        cpu_physical_memory_set_dirty_tracking(0);
    }
    else{
        DREG{printf("rsl: Error! invalid stage number %d\n", stage); 
	fflush(stdout);}
    }

    if(ret < 0) return ret;
    else return (stage == 2);
}
*/

static inline void *host_from_stream_offset(QEMUFile *f,
                                            ram_addr_t offset,
                                            int flags)
{
    static RAMBlock *block = NULL;
    char id[256];
    uint8_t len;
    // Well, since TLC don't use the CONTINUE FLAG, we may remove it later..
    if (flags & RAM_SAVE_FLAG_CONTINUE) {
        if (!block) {
            fprintf(stderr, "Ack, bad migration stream!\n");
            return NULL;
        }

        return block->host + offset;
    }

    len = qemu_get_byte(f);
    qemu_get_buffer(f, (uint8_t *)id, len);
    id[len] = 0;

    QLIST_FOREACH(block, &ram_list.blocks, next) {
        if (!strncmp(id, block->idstr, sizeof(id))){
	    return block->host + offset;
	}
    }

    fprintf(stderr, "Can't find block %s!\n", id);
    return NULL;
}

// TLC 
/*
#include "tlc.h"
#include "tlc-debug.h"

#define TLC_INCOMING_BARR_THREADS	2

extern pthread_barrier_t barr;
extern pthread_mutex_t *lock_array;
extern uint64_t	lock_array_size;

#define LOCK_MEM_TABLE(addr) \
	pthread_mutex_lock(&lock_array[(addr >> TARGET_PAGE_BITS)% lock_array_size])
#define UNLOCK_MEM_TABLE(addr) \
	pthread_mutex_unlock(&lock_array[(addr >> TARGET_PAGE_BITS) % lock_array_size])

extern uint8_t		*priority_array;
extern uint64_t	priority_array_size;
#define 	BASE_PRIORITY	0

// VIC stuffs
extern int vic_flag;
extern int vic_report(char *message);
*/
/* try move to tlc-arch_init.c
extern int state_transfer_type;
extern char chkpt_recovery_inst_str[MAX_TLC_MSERV][INST_STR_LEN];

extern void tlc_recovery_ram(int i);
extern void *tlc_recovery_t(void *t);
extern void tlc_processing_recovery_inst_str(int i);

int ram_load(QEMUFile *f, void *opaque, int version_id)
{
    ram_addr_t addr;
    int flags;
    int error;
    
    uint64_t   page_id;
    uint8_t   pri;
    uint8_t *p;
    size_t   key_length = sizeof(uint64_t); 
    size_t   value_length = sizeof(uint8_t)*TARGET_PAGE_SIZE;
    uint8_t *p_discarded;
    
    int cnt_report[4] = {0,0,0,0};
    
    if (version_id < 3 || version_id > 4) {
        return -EINVAL;
    }
    
    p_discarded = g_malloc(value_length); 

    do {
        addr = qemu_get_be64(f);
        flags = addr & ~TARGET_PAGE_MASK;
        addr &= TARGET_PAGE_MASK;
//printf("ramload: addr=0x%" PRIx64 " flag =0x%x\n", addr, flags);
//fflush(stdout);

        if (flags & RAM_SAVE_FLAG_MEM_SIZE) {
            if (version_id == 3) {
                if (addr != ram_bytes_total()) {
		    free(p_discarded); // TLC
                    return -EINVAL;
                }
            } else {
                // Synchronize RAM block list 
                char id[256];
                ram_addr_t length;
                ram_addr_t total_ram_bytes = addr;

                while (total_ram_bytes) {
                    RAMBlock *block;
                    uint8_t len;

                    len = qemu_get_byte(f);
                    qemu_get_buffer(f, (uint8_t *)id, len);
                    id[len] = 0;
                    length = qemu_get_be64(f);

                    pthread_mutex_lock(&acquire_tlc_ram_blocks); // TLC
		    QLIST_FOREACH(block, &tlc_ram_list.blocks, next) { // TLC modified
                        if (!strncmp(id, block->idstr, sizeof(id))) {
                            if (block->length != length){
			        pthread_mutex_unlock(&acquire_tlc_ram_blocks); // TLC
                                return -EINVAL;
			    }
			    pthread_mutex_unlock(&acquire_tlc_ram_blocks); // TLC
                            break;
                        }
                    }
		    pthread_mutex_unlock(&acquire_tlc_ram_blocks); // TLC

                    if (!block) {
                        fprintf(stderr, "Unknown ramblock \"%s\", cannot "
                                "accept migration\n", id);
			free(p_discarded); // TLC
                        return -EINVAL;
                    }

                    total_ram_bytes -= length;
                }
            }
        }

        if (flags & RAM_SAVE_FLAG_COMPRESS) {
            void *host;
            uint8_t ch, ch_discarded;

            //if (version_id == 3)
            host = qemu_get_ram_ptr(addr);
            //else
            //    host = host_from_stream_offset(f, addr, flags);
            if (!host) {
	        printf(" ram_load compressed: error cannot find mem block of addr 0x%" PRIx64 " \n", addr);
                exit(1);
		//return -EINVAL;
            }
	    
	    page_id = (addr >> TARGET_PAGE_BITS);
//	printf(" Bef LOCK ram_load COMPRESS > accepted page id = 0x%" PRIx64 " \n", page_id);
//fflush(stdout);
	    LOCK_MEM_TABLE(addr);

	    pri = priority_array[page_id];
	    //printf(" ram_load COMPRESS > pri[0x%" PRIx64 "] = %d lock_id = 0x%" PRIx64 "\n", 
	//		page_id, (int) pri, (page_id % lock_array_size));

	    if(pri == BASE_PRIORITY){
	//	printf(" ram_load COMPRESS > accepted page id = 0x%" PRIx64 " \n", page_id);
		cnt_report[0]++;
		
                ch = qemu_get_byte(f);
                memset(host, ch, TARGET_PAGE_SIZE);
#ifndef _WIN32
                if (ch == 0 &&
                    (!kvm_enabled() || kvm_has_sync_mmu())) {
                    qemu_madvise(host, TARGET_PAGE_SIZE, QEMU_MADV_DONTNEED);
                }
#endif
	       	       
	    }
	    else{
	  //      printf(" ram_load COMPRESS > discarded page id = 0x%" PRIx64 " \n", page_id); 

                ch_discarded = qemu_get_byte(f);
		cnt_report[1]++;
                //memset(host, ch_discarded, TARGET_PAGE_SIZE);
#ifndef _WIN32
                if (ch_discarded == 0 &&
                    (!kvm_enabled() || kvm_has_sync_mmu())) {
                    qemu_madvise(host, TARGET_PAGE_SIZE, QEMU_MADV_DONTNEED);
                }
#endif

	    }
	    UNLOCK_MEM_TABLE(addr);	    
//	printf(" After UNLOCK ram_load COMPRESS > accepted page id = 0x%" PRIx64 " \n", page_id);
//fflush(stdout);
	    

        } else if (flags & RAM_SAVE_FLAG_PAGE) {
            void *host;

            //if (version_id == 3)
            host = qemu_get_ram_ptr(addr);
            //else
            //    host = host_from_stream_offset(f, addr, flags);
            if (!host) {
	        printf(" ram_load: fatal error cannot find mem block of addr 0x%" PRIx64 " \n", addr);
                exit(1);
		//return -EINVAL;
            }

            //qemu_get_buffer(f, host, TARGET_PAGE_SIZE);
// new stuffs
	    page_id = (addr >> TARGET_PAGE_BITS);
//       printf(" Bef LOCK ram_load PAGE > accepted page id = 0x%" PRIx64 " \n", page_id);
//fflush(stdout);
	    LOCK_MEM_TABLE(addr);

	    pri = priority_array[page_id];
	    //printf(" ram_load PAGE > pri[0x%" PRIx64 "] = %d lock_id = 0x%" PRIx64 "\n", 
	//		page_id, (int) pri, (page_id % lock_array_size));

	    if(pri == BASE_PRIORITY){
	        cnt_report[2]++;
	  //      printf(" ram_load PAGE > accepted page id = 0x%" PRIx64 " \n", page_id);
	        qemu_get_buffer(f, host, TARGET_PAGE_SIZE); // the core
	    }
	    else{
	        cnt_report[3]++;
	  //      printf(" ram_load PAGE > discarded page id = 0x%" PRIx64 " \n", page_id); 
	        qemu_get_buffer(f, p_discarded, TARGET_PAGE_SIZE);
	    }

	    UNLOCK_MEM_TABLE(addr);	    
//       printf(" After UNLOCK ram_load PAGE > accepted page id = 0x%" PRIx64 " \n", page_id);
//fflush(stdout);
	    
        } 
	
	if (flags & RAM_SAVE_FLAG_SYNC_STATE) {
            int rc;

	    printf(" ram_load: sysnc state received\n");
	    fflush(stdout);
	    if(addr != 0){ // Assertion:
	        printf(" ram_load: sysnc state error! Abort\n");
		exit(1); 
	    }

            if(
              (state_transfer_type == TLM)||
              (state_transfer_type == TLM_STANDBY_DST)||
              (state_transfer_type == TLM_STANDBY_SRCDST)||
              (state_transfer_type == TLMC)||
              (state_transfer_type == TLMC_STANDBY_DST)
            ){
                // Barrier Synchronization 
                rc = pthread_barrier_wait(&barr);
                if(rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD){
                    printf("Incoming IO Thread: ram_load() could not wait on barrier\n");
                    exit(-1);
                }
            }
            else if(
              (state_transfer_type == TLC_EXEC)||
              (state_transfer_type == TLC_TCP_STANDBY_DST)
            ){
                int src_state_transfer_type = qemu_get_be32(f);
printf("ram_load: src_state_transfer = %d \n", src_state_transfer_type);
fflush(stdout);
                if(
                  (src_state_transfer_type == TLC_EXEC)||
                  (src_state_transfer_type == TLC_TCP_STANDBY_DST)
                ){
                    pthread_t *thr = NULL;
                    pthread_attr_t attr;
                    void *status;
                    int inst_len = 0, i;
                    int src_chkpt_mserv_cnt = qemu_get_be32(f);

// timing
uint64_t memretrieve_start, memretrieve_end; 
memretrieve_start = qemu_get_clock_ms(rt_clock);

printf("ram_load: src_chkpt_mserv_cnt = %d \n", src_chkpt_mserv_cnt);
fflush(stdout);
                    if(src_chkpt_mserv_cnt >= MAX_TLC_MSERV){
                        printf("ram_load: invalid chkpt_mserv_cnt = %d\n", src_chkpt_mserv_cnt);
                        exit(-1);
                    }

                    for(i = 0; i < src_chkpt_mserv_cnt; i++){
                        inst_len = qemu_get_be32(f);
printf("ram_load: inst_len = %d \n", inst_len);
                        if(inst_len >= INST_STR_LEN){
                          printf("ram_load: invalid inst_str_len = %d\n", inst_len);
                          exit(-1);
                        }
                        
                        memset(chkpt_recovery_inst_str[i], 0, INST_STR_LEN);
                        qemu_get_buffer(f, (uint8_t *)chkpt_recovery_inst_str[i], inst_len);
printf("ram_load: reocvery_str [%d] = %s \n", i, chkpt_recovery_inst_str[i]);
                    }

                    for(i = 0; i < src_chkpt_mserv_cnt; i++){
                        tlc_processing_recovery_inst_str(i);
                    }

                    thr = (pthread_t *)malloc(src_chkpt_mserv_cnt * sizeof(pthread_t));
                    pthread_attr_init(&attr);
                    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE); 

                    if(src_chkpt_mserv_cnt > 1){
                      for(i = 0; i < src_chkpt_mserv_cnt; i++){
	              // create a worker thread ;
	                pthread_create((thr+i), &attr, tlc_recovery_t, (void *)((long)i));
	              }
                      for(i = 0; i < src_chkpt_mserv_cnt; i++){
                        pthread_join(*(thr+i), &status);
                      }
                    }
                    else if(src_chkpt_mserv_cnt == 1){
                      tlc_recovery_ram(0);
                    }
                    else{
                        printf("ram_load: invalid src_chkpt_mserv_cnt = %d \n", src_chkpt_mserv_cnt);
                        exit(-1);
                    }
                    pthread_attr_destroy(&attr);
                    free(thr);
// timing                    
memretrieve_end = qemu_get_clock_ms(rt_clock);
printf("ram_load: Elasped time retrieving pages from mem server = %" PRId64 " ms\n", 
	    memretrieve_end - memretrieve_start);

                }
                else{
                    printf("ram_load: unknown src state transfer type  = %d \n", src_state_transfer_type);
                    exit(-1);
                }
                // Barrier Synchronization 
                //rc = pthread_barrier_wait(&barr);
                //if(rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD){
                //    printf("Incoming IO Thread: ram_load() could not wait on barrier\n");
                //    exit(-1);
                //}
            }
            else{
                printf("ram_load: unknown state transfer type  = %d \n", state_transfer_type);
                exit(-1);
            }

	    memset(priority_array, BASE_PRIORITY, priority_array_size);
         
	    // clear priority array
	    //memset(priority_array, BASE_PRIORITY, priority_array_size);
	    // wait for the parallel receiving to finish

            // simple lock sync 
    	    //pthread_mutex_lock(&mutex_incoming);
	    //pthread_mutex_unlock(&mutex_incoming);

    	    //pthread_mutex_lock(&mutex_incoming);
    	    //while (restored_incoming_new_dirty_pages == 0)
    	//	pthread_cond_wait(&receiving_parallel_pages_done, &mutex_incoming);
	    //pthread_mutex_unlock(&mutex_incoming);
	    // clear priority array
	       
	}
        
	error = qemu_file_get_error(f);
        if (error) {
            return error;
        }
    } while (!(flags & RAM_SAVE_FLAG_EOS));
    free(p_discarded);
  
    //"r21 cnt compz %d dis comp %d pages %d dis page %d Total = %d pages \n" 
    DVIC{
        if(vic_flag){
           char progress_string[VIC_STR_LEN];
           sprintf(progress_string,"e21 compz|dis compz|pages|dis pages|total\n");
	   vic_report(progress_string);
           sprintf(progress_string, "r21 %d %d %d %d %d\n", cnt_report[0], cnt_report[1], cnt_report[2], cnt_report[3], cnt_report[0]+cnt_report[1]+cnt_report[2]+cnt_report[3]);
	   vic_report(progress_string);
        }
        printf("e21 compz|dis compz|pages|dis pages|total\n");
        printf("r21 %d %d %d %d %d\n", cnt_report[0], cnt_report[1], cnt_report[2], cnt_report[3], 
	    cnt_report[0]+cnt_report[1]+cnt_report[2]+cnt_report[3]);
   }
	
    return 0;
}
*/
/*
extern int read_full(int fd, void *buf, size_t count);
void tlc_parallel_receiving_pages_priority_mutex(int fd);
void tlc_parallel_receiving_pages_mutexfree(int fd);
void tlc_barrier_synchronization_with_io_thread(void);

#define TLC_EOF_MARKER	0x0FF

void tlc_parallel_receiving_pages_priority_mutex(int fd){
    ram_addr_t addr;
    uint8_t  pri = 1;
    uint8_t  ori_pri;
    uint64_t page_id;
    int      tlc_eof; 
    int      received_pages = 0;
    uint8_t *p;
    size_t   key_length = sizeof(uint64_t); 
    size_t   value_length = sizeof(uint8_t)*TARGET_PAGE_SIZE;
    //uint8_t *p_discard = g_malloc(value_length);
    //p = malloc(sizeof(uint8_t) * DATA_LENGTH);

    while(1){
	//printf(" recv 1 wait readfull \n");
	//fflush(stdout); 
        if((tlc_eof = read_full(fd, &addr, key_length)) < 0){
	    printf("tlc incoming error: network read error\n");
	    break; 
	}
	//printf(" recv 1 tlc_eof 1 = %d\n", tlc_eof);
	//fflush(stdout); 
	
	if((tlc_eof == 0)||(addr == TLC_EOF_MARKER)){
	    //printf("tlc incoming: EOF 1 caught\n");
	    break;
	}
		
	addr &= TARGET_PAGE_MASK;
	page_id = (addr >> TARGET_PAGE_BITS);
//printf(" recv 1 tlc incoming: addr=0x%" PRIx64 " \n", addr);
//fflush(stdout);
	LOCK_MEM_TABLE(addr);		
//printf(" recv 1.2 tlc incoming: addr=0x%" PRIx64 " \n", addr);
//fflush(stdout);
	p = tlc_qemu_get_ram_ptr(addr);
//printf(" recv 1.3 tlc incoming: addr=0x%" PRIx64 " \n", addr);
//fflush(stdout);
	ori_pri = priority_array[page_id];
//printf(" tlc incoming1: addr=0x%" PRIx64 " pri[0x%" PRIx64 "] = %d lock_id = 0x%" PRIx64 "\n", 
			//addr, page_id, (int) ori_pri, (page_id % lock_array_size));
//fflush(stdout);
	tlc_eof = read_full(fd, p, value_length);
//printf(" recv 1.4 tlc incoming: addr=0x%" PRIx64 " \n", addr);
//fflush(stdout);
	if(ori_pri == 0){
		priority_array[page_id] = 1;	
	}
	UNLOCK_MEM_TABLE(addr);
	
	if(tlc_eof == 0){
		printf(" eof received 2 \n");
		break;
	}
	else if (tlc_eof < 0){
		printf(" error %d\n", tlc_eof);
		exit(1);
	}
	else{
//printf(" recv 1 page received tlc_eof = %d\n", tlc_eof);
//fflush(stdout);
		received_pages++;		
	}
    }
    DVIC{
        if(vic_flag){
           char progress_string[VIC_STR_LEN];
           sprintf(progress_string,"r22 %d\n", received_pages);
	   vic_report(progress_string);
        }
        printf("r22 %d\n", received_pages);
    }
        
}

void tlc_parallel_receiving_pages_mutexfree(int fd){
    ram_addr_t addr;
    uint8_t  pri = 1;
    uint8_t  ori_pri;
    uint64_t page_id;
    int      tlc_eof; 
    int      received_pages = 0;
    uint8_t *p;
    size_t   key_length = sizeof(uint64_t); 
    size_t   value_length = sizeof(uint8_t)*TARGET_PAGE_SIZE;

    int      rc;
    //uint8_t *p_discard = g_malloc(value_length);
    //p = malloc(sizeof(uint8_t) * DATA_LENGTH);

    while(1){
	//printf(" recv2 recv data\n");
	//fflush(stdout); 
        if((tlc_eof = read_full(fd, &addr, key_length)) < 0){
	    printf("tlc incoming error: network read error\n");
	    break; 
	}
	//printf(" tlc_eof 1 = %d\n", tlc_eof);
	//fflush(stdout); 
	
	if((tlc_eof == 0)||(addr == TLC_EOF_MARKER)){
	    //printf("tlc incoming: EOF 1 caught\n");
	    break;
	}
		
	addr &= TARGET_PAGE_MASK;
//printf(" tlc incoming: addr=0x%" PRIx64 " \n", addr);
//fflush(stdout);
	p = tlc_qemu_get_ram_ptr(addr);
	tlc_eof = read_full(fd, p, value_length);
	
	if(tlc_eof == 0){
		//printf(" eof received 2 \n");
		break;
	}
	else if (tlc_eof < 0){
		printf(" error %d\n", tlc_eof);
		exit(1);
	}
	else{
//		printf(" page received tlc_eof = %d\n", tlc_eof);
//		fflush(stdout);
		received_pages++;		
	}
    }
    DVIC{
        if(vic_flag){
           char progress_string[VIC_STR_LEN];
           sprintf(progress_string,"r3 %d\n", received_pages);
	   vic_report(progress_string);
        }
        printf("r3 %d\n", received_pages);
    }
        printf("r3 %d\n", received_pages);
        fflush(stdout);
    // simple lock sync    
    //pthread_mutex_unlock(&mutex_incoming);   

    //pthread_mutex_lock(&mutex_incoming);
    //restored_incoming_new_dirty_pages = 1;
    //pthread_cond_signal(&receiving_parallel_pages_done);
    //pthread_mutex_unlock(&mutex_incoming);   
    
}

void tlc_barrier_synchronization_with_io_thread(void){
    int      rc;
    rc = pthread_barrier_wait(&barr); // Barrier
    if(rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD){
        printf("Parallel Receiving Thread: could not wait on barrier\n");
        exit(-1);
    }
}
*/
#ifdef HAS_AUDIO
struct soundhw {
    const char *name;
    const char *descr;
    int enabled;
    int isa;
    union {
        int (*init_isa) (qemu_irq *pic);
        int (*init_pci) (PCIBus *bus);
    } init;
};

static struct soundhw soundhw[] = {
#ifdef HAS_AUDIO_CHOICE
#if defined(TARGET_I386) || defined(TARGET_MIPS)
    {
        "pcspk",
        "PC speaker",
        0,
        1,
        { .init_isa = pcspk_audio_init }
    },
#endif

#ifdef CONFIG_SB16
    {
        "sb16",
        "Creative Sound Blaster 16",
        0,
        1,
        { .init_isa = SB16_init }
    },
#endif

#ifdef CONFIG_CS4231A
    {
        "cs4231a",
        "CS4231A",
        0,
        1,
        { .init_isa = cs4231a_init }
    },
#endif

#ifdef CONFIG_ADLIB
    {
        "adlib",
#ifdef HAS_YMF262
        "Yamaha YMF262 (OPL3)",
#else
        "Yamaha YM3812 (OPL2)",
#endif
        0,
        1,
        { .init_isa = Adlib_init }
    },
#endif

#ifdef CONFIG_GUS
    {
        "gus",
        "Gravis Ultrasound GF1",
        0,
        1,
        { .init_isa = GUS_init }
    },
#endif

#ifdef CONFIG_AC97
    {
        "ac97",
        "Intel 82801AA AC97 Audio",
        0,
        0,
        { .init_pci = ac97_init }
    },
#endif

#ifdef CONFIG_ES1370
    {
        "es1370",
        "ENSONIQ AudioPCI ES1370",
        0,
        0,
        { .init_pci = es1370_init }
    },
#endif

#ifdef CONFIG_HDA
    {
        "hda",
        "Intel HD Audio",
        0,
        0,
        { .init_pci = intel_hda_and_codec_init }
    },
#endif

#endif /* HAS_AUDIO_CHOICE */

    { NULL, NULL, 0, 0, { NULL } }
};

void select_soundhw(const char *optarg)
{
    struct soundhw *c;

    if (*optarg == '?') {
    show_valid_cards:

        printf("Valid sound card names (comma separated):\n");
        for (c = soundhw; c->name; ++c) {
            printf ("%-11s %s\n", c->name, c->descr);
        }
        printf("\n-soundhw all will enable all of the above\n");
        exit(*optarg != '?');
    }
    else {
        size_t l;
        const char *p;
        char *e;
        int bad_card = 0;

        if (!strcmp(optarg, "all")) {
            for (c = soundhw; c->name; ++c) {
                c->enabled = 1;
            }
            return;
        }

        p = optarg;
        while (*p) {
            e = strchr(p, ',');
            l = !e ? strlen(p) : (size_t) (e - p);

            for (c = soundhw; c->name; ++c) {
                if (!strncmp(c->name, p, l) && !c->name[l]) {
                    c->enabled = 1;
                    break;
                }
            }

            if (!c->name) {
                if (l > 80) {
                    fprintf(stderr,
                            "Unknown sound card name (too big to show)\n");
                }
                else {
                    fprintf(stderr, "Unknown sound card name `%.*s'\n",
                            (int) l, p);
                }
                bad_card = 1;
            }
            p += l + (e != NULL);
        }

        if (bad_card) {
            goto show_valid_cards;
        }
    }
}

void audio_init(qemu_irq *isa_pic, PCIBus *pci_bus)
{
    struct soundhw *c;

    for (c = soundhw; c->name; ++c) {
        if (c->enabled) {
            if (c->isa) {
                if (isa_pic) {
                    c->init.init_isa(isa_pic);
                }
            } else {
                if (pci_bus) {
                    c->init.init_pci(pci_bus);
                }
            }
        }
    }
}
#else
void select_soundhw(const char *optarg)
{
}
void audio_init(qemu_irq *isa_pic, PCIBus *pci_bus)
{
}
#endif

int qemu_uuid_parse(const char *str, uint8_t *uuid)
{
    int ret;

    if (strlen(str) != 36) {
        return -1;
    }

    ret = sscanf(str, UUID_FMT, &uuid[0], &uuid[1], &uuid[2], &uuid[3],
                 &uuid[4], &uuid[5], &uuid[6], &uuid[7], &uuid[8], &uuid[9],
                 &uuid[10], &uuid[11], &uuid[12], &uuid[13], &uuid[14],
                 &uuid[15]);

    if (ret != 16) {
        return -1;
    }
#ifdef TARGET_I386
    smbios_add_field(1, offsetof(struct smbios_type_1, uuid), 16, uuid);
#endif
    return 0;
}

void do_acpitable_option(const char *optarg)
{
#ifdef TARGET_I386
    if (acpi_table_add(optarg) < 0) {
        fprintf(stderr, "Wrong acpi table provided\n");
        exit(1);
    }
#endif
}

void do_smbios_option(const char *optarg)
{
#ifdef TARGET_I386
    if (smbios_entry_add(optarg) < 0) {
        fprintf(stderr, "Wrong smbios provided\n");
        exit(1);
    }
#endif
}

void cpudef_init(void)
{
#if defined(cpudef_setup)
    cpudef_setup(); /* parse cpu definitions in target config file */
#endif
}

int audio_available(void)
{
#ifdef HAS_AUDIO
    return 1;
#else
    return 0;
#endif
}

int tcg_available(void)
{
    return 1;
}

int kvm_available(void)
{
#ifdef CONFIG_KVM
    return 1;
#else
    return 0;
#endif
}

int xen_available(void)
{
#ifdef CONFIG_XEN
    return 1;
#else
    return 0;
#endif
}
 
/*
void tlc_init_vars(void){
    // TLC: use acquire_ram_blocks to prevent data race on  
    //      ram_list.blocks data structure 
    //pthread_mutex_init(&acquire_ram_blocks, NULL); 
    pthread_mutex_init(&acquire_tlc_ram_blocks, NULL); // TLC ram
}

void tlc_init(void){    

    tlc_ram_size = tlc_last_ram_offset();
    tlc_page_size = tlc_ram_size >> TARGET_PAGE_BITS;
    tlc_dirty_size = (((int)(tlc_ram_size / ONEGIGABYTES)) + 1)* TLC_NUM_MEMLOCK_PER_1GB;
     
    DREG{printf("tlc_init: tlc ram size = %" PRId64 " page size = %" PRId64 " dirty size = %" PRId64 "\n", 
	tlc_ram_size, tlc_page_size, tlc_dirty_size); fflush(stdout);}
    
} 

void tlc_incoming_variables_init(void);

void tlc_migration_init(int flag){
    int i;
    
    tlc_init();
    if(flag == TLC_INCOMING_PAGES){
	tlc_incoming_variables_init();
        // Barrier initialization
        if(pthread_barrier_init(&barr, NULL, TLC_INCOMING_BARR_THREADS)){
            printf("Could not create a barrier\n");
            fflush(stdout);
            return;
        }
    }
    
    vm_last_ram_offset = tlc_last_ram_offset();
    lock_phys_ram_dirty = (pthread_mutex_t *) g_malloc(tlc_dirty_size * 
    				sizeof(pthread_mutex_t));
    for (i = 0; i < tlc_dirty_size; i++)
        pthread_mutex_init((lock_phys_ram_dirty+i), NULL);  

    cpu_slowdown_init();
}

void tlc_incoming_variables_init(void){
	int i;
	
	lock_array_size = (((int)(tlc_ram_size / ONEGIGABYTES)) + 1)* TLC_NUM_MEMLOCK_PER_1GB;

	lock_array = (pthread_mutex_t *) malloc(lock_array_size * 
    				sizeof(pthread_mutex_t));
    	for (i = 0; i < lock_array_size; i++)
        	pthread_mutex_init((lock_array+i), NULL);  
	
	priority_array = malloc((size_t)((tlc_ram_size / TARGET_PAGE_SIZE) + 1));
	priority_array_size = (tlc_ram_size / TARGET_PAGE_SIZE) + 1; 

	memset(priority_array, BASE_PRIORITY, priority_array_size);
	
	//restored_incoming_new_dirty_pages = 0;
}

extern pthread_mutex_t mutex_idone; 

void tlc_migration_finish(int flag){
    int i;
    
    if(flag == TLC_INCOMING_PAGES){
	for (i = 0; i < lock_array_size; i++)
        	pthread_mutex_destroy(lock_array+i);  	
	g_free(lock_array);
	g_free(priority_array);
	priority_array_size = 0; 
	//pthread_mutex_destroy(&mutex_incoming);
        pthread_barrier_destroy(&barr);
    }      
    //mthread = 0;
    for (i = 0; i < tlc_dirty_size; i++)
        pthread_mutex_destroy(lock_phys_ram_dirty+i);
    g_free(lock_phys_ram_dirty);
    pthread_mutex_destroy(&mutex_idone);
    pthread_rwlock_destroy(&dirty_page_rwlock);
}

*/
