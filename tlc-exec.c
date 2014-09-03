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
// TLC: begin 
#include "buffered_file.h"
#include "uthash.h"
#include "tlc-debug.h"
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
void		add_addr_buffer(ram_addr_t addr, uint8_t *p);
void 		*copier_t (void *t);
int 		get_num_cp_threads(void);
void 		copying_pages_in_addr_buffer(void);
int 		ram_save_to_buffer_parallel(void);

int 		put_page_to_hash(ram_addr_t addr, uint8_t *ptr);
int 		tlc_handle_dirty_ram(void);
int 		tlc_save_ram_complete_sync_flag(QEMUFile *f);

extern uint64_t pages_copied_last_epoch;

// TLC: worker thread
pthread_mutex_t mutex_save_page;
pthread_mutex_t mutex_theone;
pthread_cond_t  have_dirty;
int		nodirty = 1;
int 		stage2_done = 0;

uint64_t tlc_s22_pages_transferred = 0;
uint64_t tlc_s22_bytes_transferred = 0;
uint64_t tlc_vm_old_pages = 0;
uint64_t tlc_vm_new_pages = 0;

ram_cow_t 	*ram_cow_buffer = NULL;

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

#include "tlc.h"

int 	tlc_mserv_base_port = 0;
char 	*tlc_mserv_ip = NULL;
int     tlc_chkpt_type_flag = TLC_MEM_ADDR_CYCLIC; // default mem server mode

int	vic_flag = 0;
int 	vic_port = 0;
char 	*vic_ip = NULL;

extern int vic_report(char *message);

#define	TLC_OUTGOING_PAGES	1
#define TLC_INCOMING_PAGES	2

extern void *tlc_incoming_t(void *t);

extern int mc_create_connection(int num_conn);
extern int mc_create_connection2(int num_conn);
extern int vic_create_connection(int num_conn);

extern void tlc_incoming_variables_init(void);

// TLC migration configuration parameter strings

extern char *vic_flag_str;
extern char *vic_ip_str;
extern char *vic_port_str;
//extern int  tlc_config(char *filename);

extern int state_transfer_type; 
extern int chkpt_mserv_cnt; 

void live_storage_init(int flag, int num_thr);

void live_storage_init(int flag, int num_thr){
	
	//if(flag == TLC_INCOMING_PAGES){
        //        const char *filename = TLC_CONFIG_FILE; // TLC temporary
        //		tlc_config((char *) filename);	
	//}

	if(flag == TLC_OUTGOING_PAGES){
	    pthread_mutex_init(&mutex_save_page, NULL);
	    pthread_mutex_init(&mutex_theone, NULL);
	    pthread_cond_init(&have_dirty, NULL);
	}
	else{ // TLC_INCOMING_PAGES
            if(
              (state_transfer_type == TLM)||
              //(state_transfer_type == TLM_EXEC)|| // bogus
              (state_transfer_type == TLM_STANDBY_DST)||
              (state_transfer_type == TLM_STANDBY_SRCDST)||
              (state_transfer_type == TLMC)||
              (state_transfer_type == TLMC_STANDBY_DST)
            ){
		pthread_t mserv_thread;
		int t = 0;
//printf(" live s init() incoming");
		//if(vic_flag) vic_create_connection(1);
		//tlc_incoming_variables_init();		
        	pthread_create(&mserv_thread, NULL, tlc_incoming_t, (void *)((long)t));
        	pthread_detach(mserv_thread);
            }
	}
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
           (state_transfer_type == TLM_EXEC)||
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
                s22_mig_trans_failure = 1;
                ret = -1; // error
                goto out_putpage;
	}
	else{
		tlc_pages_sent_mserv++;
		tlc_s22_pages_transferred++;
		tlc_s22_bytes_transferred += TARGET_PAGE_SIZE;
    		ret = 1;
        }

out_putpage:    	
	return ret;
}

#define RAM_SAVE_FLAG_FULL     0x01 /* Obsolete, not used anymore */
#define RAM_SAVE_FLAG_COMPRESS 0x02
#define RAM_SAVE_FLAG_MEM_SIZE 0x04
#define RAM_SAVE_FLAG_PAGE     0x08
#define RAM_SAVE_FLAG_EOS      0x10
#define RAM_SAVE_FLAG_CONTINUE 0x20
// TLC
#define RAM_SAVE_FLAG_SYNC_STATE      0x40

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
#define QEMU_VM_SECTION_SPECIAL      0x06

extern char chkpt_recovery_inst_str[MAX_TLC_MSERV][INST_STR_LEN];

int tlc_save_ram_complete_sync_flag(QEMUFile *f)
{
    ram_addr_t current_addr = 0;

    DVERYDETAIL{printf("hashsb: in \n");
    fflush(stdout);}
    
    DREG{
    printf("QEMU_VM_SECTION_SPECIAL: %d\n", tlc_ram_section_id);
    fflush(stdout);
    }
    qemu_put_byte(f, QEMU_VM_SECTION_SPECIAL);
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
      (state_transfer_type == TLM_EXEC)||
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
printf("tlc_save_ram_complete_sync_flag: statetype = %d mserv_cnt = %d inst_len = %d reocvery_str [%d] = %s \n", 
state_transfer_type, chkpt_mserv_cnt, inst_len, i, chkpt_recovery_inst_str[i]);
fflush(stdout);
        }
    }
    else{
      printf("hsb: unknown state transfer type  = %d \n", state_transfer_type);
      exit(-1);
    }

// End section 2 here
printf("RAM_SAVE_FLAG_EOS\n");
fflush(stdout);
    qemu_put_be64(f, RAM_SAVE_FLAG_EOS);    

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
extern void mserv_base_close(void);

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
       (state_transfer_type == TLM_EXEC)||
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
    }
    else if(
       (state_transfer_type == TLM_EXEC)||
       (state_transfer_type == TLC_EXEC)||
       (state_transfer_type == TLC_TCP_STANDBY_DST)
      ){
        printf("close s2 conn: mserv_close()\n");
        mserv_close();
        if(
          (tlc_chkpt_type_flag == TLC_MEM_ADDR_CYCLIC_DOUBLE_CHANNELS) ||
          (tlc_chkpt_type_flag == TLC_MEM_ADDR_BLOCK_DOUBLE_CHANNELS)
        ){
          mserv_base_close();
        }
    }
    else{
        printf("close s2 conn: unknown state transfer type  = %d \n", state_transfer_type);
    }
}

#include "qemu-timer.h"

int64_t accum_page_saver_wait_time = 0; 

void *page_saver_t(void *args){
    int save_status = 0;
    int64_t start_tmp_timer, end_tmp_timer; 
    accum_page_saver_wait_time = 0; 

    while(1){
        pthread_mutex_lock(&mutex_save_page);
	while(nodirty){
                start_tmp_timer = qemu_get_clock_ms(rt_clock);

		pthread_cond_wait(&have_dirty, &mutex_save_page);

                end_tmp_timer = qemu_get_clock_ms(rt_clock);
                accum_page_saver_wait_time += 
                          (end_tmp_timer - start_tmp_timer);
	}

	if(idone){
            tlc_close_s2_connections();
	    stage2_done = 1;

            pthread_mutex_lock(&mutex_theone);

            pthread_mutex_unlock(&mutex_save_page);

            pthread_mutex_lock(&mutex_theone);
            pthread_mutex_unlock(&mutex_theone);

            if(
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

            if (s22_mig_trans_failure == 0){ 
                // if fail during stage 2, wait for 2.1 to bail 
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

//addr_buffer_t * add_addr_buffer(ram_addr_t addr, uint8_t *p){
void add_addr_buffer(ram_addr_t addr, uint8_t *p){
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
    
    //return new_addr_item;
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
    if(
       (state_transfer_type == TLMC_STANDBY_DST)||
       (state_transfer_type == TLMC)
      ){
        my_memory_buffer = my_ram_contents->memory_buffer;
    }
    
    DREG{printf("copier_t: my_addr_index(start)=%d, num=%d\n", 
    	my_addr_index, my_addr_num);
    fflush(stdout);}
	    
    if(
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
    else if(
            (state_transfer_type == TLM_EXEC)||
            (state_transfer_type == TLC_EXEC)||
            (state_transfer_type == TLC_TCP_STANDBY_DST)
        ){ // TLC
        for(k = 0; k < my_addr_num; k ++){
            current_addr_buffer = addr_buffer+my_addr_index;
	    saved_ptr = current_addr_buffer->ptr;
	    my_addr_contents = current_addr_buffer->mem_contents;	
	    mkey = current_addr_buffer->addr;	

	    // send page to membase servers
	    ret= mserv_set((const char *) &mkey, key_length, (const char *) saved_ptr, value_length);
	
	    if (ret < 0){
		printf("copier_t: Couldn't store key to mserv:ret = %d \n", ret);
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

        if(
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
    if(
       (state_transfer_type == TLMC_STANDBY_DST)||
       (state_transfer_type == TLMC)
      ){
        my_memory_buffer = my_ram_contents->memory_buffer;
    }
    
    DREG{printf("cp_buffer: cptid=%ld, my_addr_index(start)=%d, num=%d\n", 
    	my_cp_tid, my_addr_index, my_addr_num);
    fflush(stdout);}

    // VIC stuffs
    //if(vic_flag){
        vic_local_counter = 0;
        vic_one_tenth = (int)(my_addr_num/10);
	vic_accumulate = 0;
    //}

    if(
       (state_transfer_type == TLMC_STANDBY_DST)||
       (state_transfer_type == TLMC)
      ){
        for(k = 0; k < my_addr_num; k++){
            current_addr_buffer = addr_buffer+my_addr_index; 
	    saved_ptr = current_addr_buffer->ptr;
	    mkey = current_addr_buffer->addr;	
        
	    memcpy(my_memory_buffer, saved_ptr, value_length);

	    current_addr_buffer->mem_contents = my_memory_buffer;	
            my_memory_buffer += value_length;

	    // VIC stuffs
	    vic_local_counter++;
	    if(vic_local_counter >= vic_one_tenth){
		char progress_string[VIC_STR_LEN];

		vic_accumulate += 10;
		vic_local_counter = 0;  
	    	if(vic_flag){
	    		snprintf(progress_string, VIC_STR_LEN,"st %d", vic_accumulate);
	    		vic_report(progress_string);
	    	}	
	    	printf(".. %d ", vic_accumulate);
		fflush(stdout);
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
	    	vic_local_counter++;
	    	if(vic_local_counter >= vic_one_tenth){
			char progress_string[VIC_STR_LEN];

			vic_accumulate += 10;
			vic_local_counter = 0;  
	    		if(vic_flag){
	    			snprintf(progress_string, VIC_STR_LEN,"st %d", vic_accumulate);
	    			vic_report(progress_string);
	    		}	
	    		printf(".. %d ", vic_accumulate);
			fflush(stdout);
	    	}
	   }	
	   my_addr_index++;
        }
    }
    else if( 
            (state_transfer_type == TLC_EXEC)||
            (state_transfer_type == TLM_EXEC)||
            (state_transfer_type == TLC_TCP_STANDBY_DST)
        ){ // TLC
        for(k = 0; k < my_addr_num; k++){
            current_addr_buffer = addr_buffer+my_addr_index; 
	    saved_ptr = current_addr_buffer->ptr;
	    my_addr_contents = current_addr_buffer->mem_contents;	
	    mkey = current_addr_buffer->addr;	
        
	    // send page to mem servers
	    ret= mserv_set((const char *) &mkey, key_length, (const char *) saved_ptr, value_length);
	
	    if (ret < 0){
		printf("cp_buffer: Couldn't store to mserver key:ret = %d \n", ret);
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
	    	vic_local_counter++;
	    	if(vic_local_counter >= vic_one_tenth){
			char progress_string[VIC_STR_LEN];

			vic_accumulate += 10;
			vic_local_counter = 0;  
	    		if(vic_flag){
	    			snprintf(progress_string, VIC_STR_LEN,"st %d", vic_accumulate);
	    			vic_report(progress_string);
	    		}	
	    		printf(".. %d ", vic_accumulate);
			fflush(stdout);
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
        printf("Percents\n");
	fflush(stdout);
  
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

uint64_t tlc_ram_list_copied = 0;
uint64_t ram_list_copied = 0;

    addr_buffer_init();
    
    while (addr < vm_last_ram_offset) {

        if (tlc_lock_free_cpu_physical_memory_get_dirty(addr, MIGRATION_DIRTY_FLAG)) {
            uint8_t *p;
	    
	    p = tlc_qemu_get_ram_ptr(addr);  
	    add_addr_buffer(addr, p);
tlc_ram_list_copied++;
        }
        else if (cpu_physical_memory_get_dirty(addr, MIGRATION_DIRTY_FLAG)) {
            uint8_t *p;
	    
	    p = tlc_qemu_get_ram_ptr(addr);  
	    add_addr_buffer(addr, p);
ram_list_copied++;
        }
        addr += TARGET_PAGE_SIZE;
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
               (state_transfer_type == TLMC)||
               (state_transfer_type == TLMC_STANDBY_DST)
              ){
	         // send page to mem servers
	         ret = mc_set2((const char *) &mkey, key_length, (const char *) my_addr_contents, value_length);
            }
            else if(
               (state_transfer_type == TLC_EXEC)||
               (state_transfer_type == TLM_EXEC)||
               (state_transfer_type == TLC_TCP_STANDBY_DST)||
               (state_transfer_type == TLM)||
               (state_transfer_type == TLM_STANDBY_DST)||
               (state_transfer_type == TLM_STANDBY_SRCDST)
              ){
	         printf("transfer_contents: error TLM (%d) should not reach this function\n", state_transfer_type);
                 fflush(stdout);
                 s3_mig_trans_failure = 1;
                 goto out_trans_buffer;
            }
            else{
	         printf("transfer_contents: unknown state transfer type  = %d \n", state_transfer_type);
                 fflush(stdout);
                 s3_mig_trans_failure = 1;
                 goto out_trans_buffer;
            }
	
	    if (ret < 0){
		printf("transfer_contents: Couldn't transfer key:ret = %d \n", ret);
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

