/*
 * QEMU live migration
 *
 * Copyright IBM, Corp. 2008
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#include "qemu-common.h"
#include "migration.h"
#include "monitor.h"
#include "buffered_file.h"
#include "sysemu.h"
#include "block.h"
#include "qemu_socket.h"
#include "block-migration.h"
#include "qmp-commands.h"

#include "tlc.h"

//#define DEBUG_MIGRATION

#ifdef DEBUG_MIGRATION
#define DPRINTF(fmt, ...) \
    do { printf("migration: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

enum {
    MIG_STATE_ERROR,
    MIG_STATE_SETUP,
    MIG_STATE_CANCELLED,
    MIG_STATE_ACTIVE,
    MIG_STATE_COMPLETED,
};

#define MAX_THROTTLE  (32 << 20)      /* Migration speed throttling */

static NotifierList migration_state_notifiers =
    NOTIFIER_LIST_INITIALIZER(migration_state_notifiers);

// TLC declarations
#include 	<pthread.h>
#include 	"tlc-debug.h"

int64_t 	mig_start, mig_finish;
int64_t 	mig_period_start, mig_period_end, 
		mig_duration, acc_vm_overhead, max_mig_duration;
int 		duration_id;
int64_t 	stage_start, stage_finish;

int64_t 	epoch_start, epoch_end, epoch_duration;
uint64_t 	pages_copied_last_epoch; 
uint64_t 	pages_gen_last_epoch;

uint64_t 	pages_transferred;

int 		ncpus;
int		mthread = 0;
uint64_t	do_migrate_id = 0;
uint64_t	chkp_thread_id = 0;
int		idone = 0;

pthread_mutex_t vm_waitfor_checkpoint_thread;
pthread_mutex_t mutex_idone;
pthread_cond_t  save_dev_state_done;
int 		save_dev_state; 

pthread_rwlock_t dirty_page_rwlock;

extern uint64_t tlc_vm_written_page_count;
extern uint64_t tlc_vm_written_byte_count;
extern uint64_t tlc_vm_hash_table_add;
extern uint64_t tlc_vm_hash_table_excluded;

// TLC: begin
extern void tlc_migration_init(int flag);
extern void tlc_migration_finish(int flag);
extern void *page_saver_t(void *args);
int final_migration_cleanup(MigrationState *s);
// TLC: end
extern int vic_flag;
extern int vic_report(char *message);

int state_transfer_type = NOTYPE; 

static MigrationState *migrate_get_current(void)
{
    static MigrationState current_migration = {
        .state = MIG_STATE_SETUP,
        .bandwidth_limit = MAX_THROTTLE,
    };

    return &current_migration;
}

int qemu_start_incoming_migration(const char *uri)
{
    const char *p;
    int ret;

    state_transfer_type = NOTYPE; 

    // on destination
    if (strstart(uri, "tcp:", &p)){
        ret = tcp_start_incoming_migration(p);
    }
#if !defined(WIN32)
    else if (strstart(uri, "tlc-exec:", &p)){ 
        state_transfer_type = TLC_EXEC;
        autostart = 0;
        ret = exec_start_incoming_migration(p);
    }
    else if (strstart(uri, "tlc-tcp-standby-dst:", &p)){
        state_transfer_type = TLC_TCP_STANDBY_DST;
        autostart = 0; 
        ret = tcp_start_incoming_migration(p);
    }
    else if (strstart(uri, "tlm:", &p)){
        state_transfer_type = TLM;
        autostart = 1; 
        ret = tcp_start_incoming_migration(p);
    }
    else if (strstart(uri, "tlmc:", &p)){
        state_transfer_type = TLMC;
        //autostart = 0;
        autostart = 1; // temporary.. testing
        ret = tcp_start_incoming_migration(p);
    }
    else if (strstart(uri, "tlmc-standby-dst:", &p)){
        state_transfer_type = TLMC_STANDBY_DST;
        autostart = 0;
        //autostart = 1; 
        ret = tcp_start_incoming_migration(p);
    }
    else if (strstart(uri, "tlm-standby-dst:", &p)){
        state_transfer_type = TLM_STANDBY_DST;
        autostart = 0; 
        ret = tcp_start_incoming_migration(p);
    }
    else if (strstart(uri, "tlm-standby-srcdst:", &p)){
        state_transfer_type = TLM_STANDBY_SRCDST;
        autostart = 0; 
        ret = tcp_start_incoming_migration(p);
    }
    else if (strstart(uri, "exec:", &p)){
        ret = exec_start_incoming_migration(p);
    }
    else if (strstart(uri, "unix:", &p)){
        ret = unix_start_incoming_migration(p);
    }
    else if (strstart(uri, "fd:", &p)){
        ret = fd_start_incoming_migration(p);
    }
#endif
    else {
        fprintf(stderr, "unknown migration protocol: %s\n", uri);
        ret = -EPROTONOSUPPORT;
    }
    return ret;
}

void process_incoming_migration(QEMUFile *f)
{
    if (qemu_loadvm_state(f) < 0) {
        fprintf(stderr, "load of migration failed\n");
        exit(0);
    }
    qemu_announce_self();
    DVIC{
        if(vic_flag){
           char progress_string[VIC_STR_LEN];
           sprintf(progress_string,"r4 successfully loaded vm state");
	   vic_report(progress_string);
        }
    }
    printf("successfully loaded vm state\n");

    /* Make sure all file formats flush their mutable metadata */
    bdrv_invalidate_cache_all();

    if (autostart) {
        vm_start();
    } else {
        runstate_set(RUN_STATE_PRELAUNCH);
    }
}

/* amount of nanoseconds we are willing to wait for migration to be down.
 * the choice of nanoseconds is because it is the maximum resolution that
 * get_clock() can achieve. It is an internal measure. All user-visible
 * units must be in seconds */
static uint64_t max_downtime = 30000000;

uint64_t migrate_max_downtime(void)
{
    return max_downtime;
}

MigrationInfo *qmp_query_migrate(Error **errp)
{
    MigrationInfo *info = g_malloc0(sizeof(*info));
    MigrationState *s = migrate_get_current();

    switch (s->state) {
    case MIG_STATE_SETUP:
        /* no migration has happened ever */
        break;
    case MIG_STATE_ACTIVE:
        info->has_status = true;
        info->status = g_strdup("active");

        info->has_ram = true;
        info->ram = g_malloc0(sizeof(*info->ram));
        info->ram->transferred = ram_bytes_transferred();
        info->ram->remaining = ram_bytes_remaining();
        info->ram->total = ram_bytes_total();

        if (blk_mig_active()) {
            info->has_disk = true;
            info->disk = g_malloc0(sizeof(*info->disk));
            info->disk->transferred = blk_mig_bytes_transferred();
            info->disk->remaining = blk_mig_bytes_remaining();
            info->disk->total = blk_mig_bytes_total();
        }
        break;
    case MIG_STATE_COMPLETED:
        info->has_status = true;
        info->status = g_strdup("completed");
        break;
    case MIG_STATE_ERROR:
        info->has_status = true;
        info->status = g_strdup("failed");
        break;
    case MIG_STATE_CANCELLED:
        info->has_status = true;
        info->status = g_strdup("cancelled");
        break;
    }

    return info;
}

/* shared migration helpers */

static void migrate_fd_monitor_suspend(MigrationState *s, Monitor *mon)
{
    if (monitor_suspend(mon) == 0) {
        DPRINTF("suspending monitor\n");
	
	DREG{printf("suspending monitor\n");
	fflush(stdout);}
    } else {
        monitor_printf(mon, "terminal does not allow synchronous "
                       "migration, continuing detached\n");
    }
}

static int migrate_fd_cleanup(MigrationState *s)
{
    int ret = 0;
    
    qemu_set_fd_handler2(s->fd, NULL, NULL, NULL, NULL);

    if (s->file) {
        if (qemu_fclose(s->file) != 0) {
            ret = -1;
	    DREG{printf("mfcleanup: close return ret = %d\n", ret);}
        }
        s->file = NULL;
    } else {
        if (s->mon) {
	    DREG{printf("mfcleanup: resume monitor\n");
	    fflush(stdout);}
            monitor_resume(s->mon);
        }
    }

    if (s->fd != -1) {
	DREG{printf("mfcleanup: close(s->fd)\n");
	fflush(stdout);}
        close(s->fd);
        s->fd = -1;
    }

    return ret;
}

void migrate_fd_error(MigrationState *s)
{
    s->state = MIG_STATE_ERROR;
    notifier_list_notify(&migration_state_notifiers, s);
    migrate_fd_cleanup(s);
}

static void migrate_fd_completed(MigrationState *s)
{
    if (migrate_fd_cleanup(s) < 0) {
        s->state = MIG_STATE_ERROR;
    } else {
        s->state = MIG_STATE_COMPLETED;
    }

    notifier_list_notify(&migration_state_notifiers, s);
    
}

static void migrate_fd_put_notify(void *opaque)
{
    MigrationState *s = opaque;

    qemu_set_fd_handler2(s->fd, NULL, NULL, NULL, NULL);
    qemu_file_put_notify(s->file);
    
}

int s21_mig_trans_failure;
int s22_mig_trans_failure;
int s3_mig_trans_failure;

static ssize_t migrate_fd_put_buffer(void *opaque, const void *data,
                                     size_t size)
{
    MigrationState *s = opaque;
    ssize_t ret;
    
    if (s->state != MIG_STATE_ACTIVE) {
        DREG{printf("mfputbuffer out: not mig state active\n");
	fflush(stdout);}
        return -EIO;
    }

    do {
        ret = s->write(s, data, size);
    } while (ret == -1 && ((s->get_error(s)) == EINTR));

    if (ret == -1){

        ret = -(s->get_error(s));

        s21_mig_trans_failure = 1; // original migration falis

	printf("mfputbuffer: ret s->geterror %d\n", (int)ret);
	fflush(stdout);
        
    }

    if (ret == -EAGAIN) {
        /*
	DDETAIL{printf("mfputbuffer: set handler mig fd put norify() ret was %d\n", (int)ret);
	fflush(stdout);}

        qemu_set_fd_handler2(s->fd, NULL, NULL, migrate_fd_put_notify, s);
        */
    }
    return ret;
}

static void migrate_fd_put_ready(void *opaque)
{
    pthread_t this_thread_id = 0;
    MigrationState *s = opaque;
    int ret;
    
    if (s->state != MIG_STATE_ACTIVE) {
        printf("error! put_ready returning because of non-active state\n");
	fflush(stdout);
        return;
    }

    this_thread_id = pthread_self();
    
    if(this_thread_id == chkp_thread_id){ 
    	if(idone){
		printf("mfpr error! on chkp_thread, idone was set!\n");
		fflush(stdout);
	}
    	ret = qemu_savevm_state_iterate(s->mon, s->file);
    	if (ret < 0) {
        	migrate_fd_error(s);
	}
    } 
    else if (this_thread_id == do_migrate_id){
	DREG{stage_start = qemu_get_clock_ms(rt_clock);}
		
        vm_stop_force_state(RUN_STATE_FINISH_MIGRATE);

        if (qemu_savevm_state_complete(s->mon, s->file) < 0) {
        	DREG{printf("Stage 3 Error set MIG_STATE_ERROR\n");
		fflush(stdout);}
		s->state = MIG_STATE_ERROR;
		vm_start();
        } 
        else{ // TLC_TLM_COMBO
                switch(state_transfer_type){
                case TLC_EXEC: 
                case TLC_TCP_STANDBY_DST: 
                case TLM_STANDBY_DST: 
                case TLMC_STANDBY_DST: 
                    vm_start(); // TLC, let the vm continue.
                    break;
                case TLM: 
                case TLMC: 
                case TLM_STANDBY_SRCDST: 
                    // temporary testing
                    printf("pause source vm!\n");
	            fflush(stdout);
                    break;
                    // vm_start(); // if TLC, let the vm continue.
                // Otherwise, if the type is TLM, stop this vm.
                }
        }
	
        DREG{stage_finish = qemu_get_clock_ms(rt_clock);
	printf("Stage 3 Elapsed time: %" PRId64 " ms\n", stage_finish - stage_start);
	fflush(stdout);}
	
    }
    else{
	printf("Stage 3 Error! mfpr was invoked on an unknown thread!\n");
	fflush(stdout);
    }
}

static void migrate_fd_cancel(MigrationState *s)
{
    if (s->state != MIG_STATE_ACTIVE)
        return;

    s->state = MIG_STATE_CANCELLED;
    notifier_list_notify(&migration_state_notifiers, s);
    qemu_savevm_state_cancel(s->mon, s->file);

    migrate_fd_cleanup(s);
}

static void migrate_fd_wait_for_unfreeze(void *opaque)
{
    MigrationState *s = opaque;
    int ret;

    //printf("wait for unfreeze\n");
    if (s->state != MIG_STATE_ACTIVE)
        return;

    do {
        fd_set wfds;

        FD_ZERO(&wfds);
        FD_SET(s->fd, &wfds);

        ret = select(s->fd + 1, NULL, &wfds, NULL, NULL);
    } while (ret == -1 && (s->get_error(s)) == EINTR);

    if (ret == -1) {
	DREG{printf("mfwaitunfreeze: set file error\n");
	fflush(stdout);}
        qemu_file_set_error(s->file, -s->get_error(s));
    }
}

static int migrate_fd_close(void *opaque)
{
    MigrationState *s = opaque;

    if (s->mon) {
	
        monitor_resume(s->mon);
    }

    qemu_set_fd_handler2(s->fd, NULL, NULL, NULL, NULL);
    return s->close(s);
}

void add_migration_state_change_notifier(Notifier *notify)
{
    notifier_list_add(&migration_state_notifiers, notify);
}

void remove_migration_state_change_notifier(Notifier *notify)
{
    notifier_list_remove(&migration_state_notifiers, notify);
}

bool migration_is_active(MigrationState *s)
{
    return s->state == MIG_STATE_ACTIVE;
}

bool migration_has_finished(MigrationState *s)
{
    return s->state == MIG_STATE_COMPLETED;
}

bool migration_has_failed(MigrationState *s)
{
    return (s->state == MIG_STATE_CANCELLED ||
            s->state == MIG_STATE_ERROR);
}

extern uint64_t bytes_transferred;
extern int tlc_pages_sent_mserv; 

#define	TLC_OUTGOING_PAGES	1
#define TLC_INCOMING_PAGES	2

int final_migration_cleanup(MigrationState *s)
{
DREG{	
printf("Total #pages sent to dst during Stage 2.2 = %d pages\n", tlc_pages_sent_mserv);
printf("Total #pages trans to dst or local HT during Stage 2.2 = %" PRId64 " pages\n", tlc_vm_written_page_count);
printf("Total Amt of Data Trans to dst or local HT during Stage 2.2 = %" PRId64 " kbytes\n", tlc_vm_written_byte_count >> 10);
printf("Total Pages Sent on Stage 2.1 plus Stage 4 (from local HT) = %" PRIu64 " pages\n", pages_transferred);
printf("Total Bytes Sent on Stage 2.1 plus Stage 4 = %" PRIu64 " kbytes\n", ram_bytes_transferred() >> 10);
}	
	
	migrate_fd_completed(s); // fflush buffer to file (take time)
	tlc_migration_finish(TLC_OUTGOING_PAGES);
	
        DVIC{
	    char progress_string[VIC_STR_LEN];
            int64_t vic_mig_thread_finish = qemu_get_clock_ms(rt_clock);

            if(vic_flag){
                sprintf(progress_string, "s4 %d %" PRId64 " %" PRIu64 " ", tlc_pages_sent_mserv, pages_transferred, (vic_mig_thread_finish - mig_start));
	        vic_report(progress_string);
            }
            //printf("PSsent %d : CPsent  %" PRId64 ": ElaspMig %" PRIu64 "\n", tlc_pages_sent_mserv, pages_transferred, (vic_mig_thread_finish - mig_start));
            printf("s4 %d %" PRId64 " %" PRIu64 "\n", tlc_pages_sent_mserv, pages_transferred, (vic_mig_thread_finish - mig_start));
        }
        
	bytes_transferred = 0;
	pages_transferred = 0;

	mthread = 0;
	return 0;
}

extern int nodirty; 
extern pthread_cond_t  have_dirty;
extern pthread_mutex_t mutex_save_page;
extern int qemu_save_hash(QEMUFile *f);

void migrate_fd_connect(MigrationState *s)
{
    int ret;
    
    s->state = MIG_STATE_ACTIVE;
    s->file = qemu_fopen_ops_buffered(s,
                                      s->bandwidth_limit,
                                      migrate_fd_put_buffer,
                                      migrate_fd_put_ready,
                                      migrate_fd_wait_for_unfreeze,
                                      migrate_fd_close);

    DPRINTF("beginning savevm\n");
    
    ret = qemu_savevm_state_begin(s->mon, s->file, s->blk, s->shared);
    if (ret < 0) {
        DREG{printf("mfc: qssb failed, %d\n", ret);}
        migrate_fd_error(s);
        return;
    }

    pthread_mutex_unlock(&vm_waitfor_checkpoint_thread);
    
    migrate_fd_put_ready(s);
    
    pthread_mutex_lock(&mutex_save_page);
    idone = 1; // use this to tell the PS thread that chkp thr finishes stage2
    nodirty = 0;
    pthread_cond_signal(&have_dirty); // in case the PS is waiting
    pthread_mutex_unlock(&mutex_save_page);

    pthread_mutex_lock(&mutex_idone);
    while (save_dev_state == 0)
    	pthread_cond_wait(&save_dev_state_done, &mutex_idone);
	// Wait until stage 3 on the vm thread is done
    pthread_mutex_unlock(&mutex_idone); 
    
    stage_start = qemu_get_clock_ms(rt_clock);
    qemu_save_hash(s->file);
    stage_finish = qemu_get_clock_ms(rt_clock);	

    if(s3_mig_trans_failure){
        printf("migrate_fd_connect: io_thread: Stage 4 error s3_failure is set\n"); 
    }
    else{
        DREG{printf("Stage 4 Elapsed time sending local HashTable to dst: %" PRId64 " ms\n",
	    stage_finish - stage_start);
        printf("Stage 4 Elasped time from start to completion of Stage 4: %" PRId64 " ms\n", 
	    stage_finish - mig_start);}
    }

    final_migration_cleanup(s); // this will also fflush contents
    
}

static MigrationState *migrate_init(Monitor *mon, int detach, int blk, int inc)
{
    MigrationState *s = migrate_get_current();
    int64_t bandwidth_limit = s->bandwidth_limit;

    memset(s, 0, sizeof(*s));
    s->bandwidth_limit = bandwidth_limit;
    s->blk = blk;
    s->shared = inc;

    /* s->mon is used for two things:
       - pass fd in fd migration
       - suspend/resume monitor for not detached migration
    */
    s->mon = mon;
    s->bandwidth_limit = bandwidth_limit;
    s->state = MIG_STATE_SETUP;

    if (!detach) {
        migrate_fd_monitor_suspend(s, mon);
    }
    return s;
}

static GSList *migration_blockers;

void migrate_add_blocker(Error *reason)
{
    migration_blockers = g_slist_prepend(migration_blockers, reason);
}

void migrate_del_blocker(Error *reason)
{
    migration_blockers = g_slist_remove(migration_blockers, reason);
}

// Global vars to pass values to do_migrate_t
typedef struct {
    MigrationState *s;
    Monitor *mon;
    int detach;
    const char *uri;
} Tmig;

Tmig t;

int  tlc_chkpt_state_flag = TLC_CHKPT_INIT;

static int do_migrate_state(MigrationState *s, Monitor *mon, int detach, const char *uri)
{
    int ret;
    const char *p;
    
    state_transfer_type = NOTYPE; 

    // on source
    if (strstart(uri, "tlc-exec:", &p)) {
        state_transfer_type = TLC_EXEC;
        if (tlc_chkpt_state_flag == TLC_CHKPT_ABORT){
          ret = TLC_CHKPT_ABORT;
          goto mig_state_out;
        }
        ret = exec_start_outgoing_migration(s, p);
    } else if (strstart(uri, "tlc-tcp-standby-dst:", &p)) {
        state_transfer_type = TLC_TCP_STANDBY_DST;
        if (tlc_chkpt_state_flag == TLC_CHKPT_ABORT){
          ret = TLC_CHKPT_ABORT;
          goto mig_state_out;
        }
        ret = tcp_start_outgoing_migration(s, p);
    } else if (strstart(uri, "tlm:", &p)) {
        state_transfer_type = TLM;
        ret = tcp_start_outgoing_migration(s, p);
    } else if (strstart(uri, "tlmc:", &p)) { // temporary
        state_transfer_type = TLMC;
        ret = tcp_start_outgoing_migration(s, p);
    } else if (strstart(uri, "tlmc-standby-dst:", &p)) {
        state_transfer_type = TLMC_STANDBY_DST;
        ret = tcp_start_outgoing_migration(s, p);
    } else if (strstart(uri, "tlm-standby-dst:", &p)) {
        state_transfer_type = TLM_STANDBY_DST;
        ret = tcp_start_outgoing_migration(s, p);
    } else if (strstart(uri, "tlm-standby-srcdst:", &p)) {
        state_transfer_type = TLM_STANDBY_SRCDST;
        ret = tcp_start_outgoing_migration(s, p);
    } else {
        DREG{printf("unknown migration protocol: %s\n", uri);}
        ret  = -EINVAL;
    }

mig_state_out: 
    if (ret < 0) {
        printf("migration failed: %d\n", ret);
        migrate_fd_cancel(s);
    }
    return ret;
}

static void *do_migrate_t(void *arg)
{
    int64_t mig_thread_start, mig_thread_finish;
    char progress_string[VIC_STR_LEN];
    
    chkp_thread_id = pthread_self(); // for use in mfpr 
    
    mig_thread_start = qemu_get_clock_ms(rt_clock);
    do_migrate_state(t.s, t.mon, t.detach, t.uri);
    mig_thread_finish = qemu_get_clock_ms(rt_clock);
    
    DREG{
    printf("do_mig_t: checkpoint thread exe time: %" PRId64 " ms\n        Elasp time from start of migration %" PRId64 " ms\n", 
		(mig_thread_finish - mig_thread_start), (mig_thread_finish - mig_start));
    fflush(stdout);
    }
    pthread_exit((void *)0);
}

#define	TLC_OUTGOING_PAGES	1
#define TLC_INCOMING_PAGES	2
extern void live_storage_init(int flag, int num_thr);
extern void tlc_clear_all_dirty_flags(void);
extern uint64_t tlc_ram_page_remaining(void);
extern void tlc_display_ram_list(void);

// TLC bookkeepping
double epoch_pages_gen_speed; //  = pages gen/epoch duration
double aggregate_pages_gen;
double avg_pages_gen_per_epoch; // = agg pages gen/num of epoch

double epoch_pages_sent_speed; // = pages sent/epoch duration
double aggregate_pages_sent;
double avg_pages_sent_per_epoch; // = agg pages sent/num of epoch

double aggregate_epoch_duration; 

double avg_pages_gen_speed;  // = agg pages gen/agg epoch duration
double avg_pages_sent_speed;  // = agg pages sent/agg epoch duration

int do_migrate(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    MigrationState *s = migrate_get_current();
    
    int detach = qdict_get_try_bool(qdict, "detach", 0);
    int blk = qdict_get_try_bool(qdict, "blk", 0);
    int inc = qdict_get_try_bool(qdict, "inc", 0);
    const char *uri = qdict_get_str(qdict, "uri");

    ncpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    
    if (s->state == MIG_STATE_ACTIVE) {
        monitor_printf(mon, "migration already in progress\n");
        return -1;
    }

    if (qemu_savevm_state_blocked(mon)) {
        return -1;
    }

    if (migration_blockers) {
        Error *err = migration_blockers->data;
        qerror_report_err(err);
        return -1;
    }
    live_storage_init(TLC_OUTGOING_PAGES, 6); // We can do this before migration (timing).
    // In case og tlc, no connection will be created in above function 'cos 
    // has already been created in tlc_config()

    // FT flags
    s21_mig_trans_failure = 0;
    s22_mig_trans_failure = 0;
    s3_mig_trans_failure = 0;
    // TLC bookkeeping
    epoch_pages_gen_speed = 0; 
    aggregate_pages_gen = 0;
    avg_pages_gen_per_epoch = 0; 

    epoch_pages_sent_speed = 0; 
    aggregate_pages_sent = 0;
    avg_pages_sent_per_epoch = 0; 

    aggregate_epoch_duration = 0; 

    avg_pages_gen_speed = 0;  
    avg_pages_sent_speed = 0;  

    mig_period_start = mig_start = qemu_get_clock_ms(rt_clock);
    acc_vm_overhead = max_mig_duration = duration_id = 0;    

    tlc_clear_all_dirty_flags(); // TLC: this must be called before set mthread = 1

    s = migrate_init(mon, detach, blk, inc);   
    mthread = 1;

    //tlc_display_ram_list(); // TLC ram
    
    tlc_migration_init(TLC_OUTGOING_PAGES); // TLC
    pthread_mutex_init(&vm_waitfor_checkpoint_thread, NULL);
    
    pthread_mutex_init(&mutex_idone, NULL);
//  TLC reader/writer mutex
    pthread_rwlock_init(&dirty_page_rwlock, NULL);
    // we use this rwlock to reduce tlc_ram_sync time

    if(ncpus > 1){
	pthread_t thread;
	pthread_t worker;

	DREG{printf("dm: Found %d cores\n", ncpus); 
	fflush(stdout);}

	t.s = s;
	t.mon = mon;
        t.detach = detach;
        t.uri = uri;
        
	// TLC stat
	pages_copied_last_epoch = 0;
    	pages_gen_last_epoch = 0;
	
	pthread_mutex_lock(&mutex_idone);
	idone = 0;
	pthread_mutex_unlock(&mutex_idone);
	
	pthread_mutex_lock(&vm_waitfor_checkpoint_thread);
	
        do_migrate_id = pthread_self(); // keep id of the VM thread
	
	pthread_create(&worker, NULL, page_saver_t, NULL); 
        pthread_create(&thread, NULL, do_migrate_t, NULL);
        pthread_detach(thread);
	pthread_detach(worker);

//test_timer_stop3 = qemu_get_clock_ms(rt_clock);
//DREG{printf("test_timer3: elasp =  %" PRId64 " ms\n", test_timer_stop3 - test_timer_stop2);
//fflush(stdout);}
	//block here until the vm_waitfor_checkpoint_thread is released by do_migrate_t thread
	pthread_mutex_lock(&vm_waitfor_checkpoint_thread);
	pthread_mutex_unlock(&vm_waitfor_checkpoint_thread);
        
    } else {
        DREG{printf("dm: Found only one core\n"); 
	fflush(stdout);}
    }
    mig_period_end = qemu_get_clock_ms(rt_clock);
    mig_duration = mig_period_end - mig_period_start;
    acc_vm_overhead += mig_duration;
    if(max_mig_duration < mig_duration) max_mig_duration = mig_duration;
        
    epoch_start = mig_period_end;
    
    DREG{printf("dm: id: %d: duration: %" PRId64 " ms\n\n", duration_id, mig_duration);
    fflush(stdout);}
    
    if (detach) {
        s->mon = NULL;
    }

    notifier_list_notify(&migration_state_notifiers, s);
    
    return 0;
}

int do_migrate_cancel(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    migrate_fd_cancel(migrate_get_current());
    return 0;
}

// TLC migration configuration parameter strings

char *tlc_mserv_ip_str = NULL;
char *tlc_mserv_base_port_str = NULL;
char *tlc_interval_str = NULL;
char *tlc_slowdown_interval_str = NULL;
char *tlc_slowdown_trigger_str = NULL;
char *tlc_slowdown_stepwise_str = NULL;
char *tlc_slowdown_aggressiveness_str = NULL;
char *tlc_slowdown_marker_str = NULL;
char *vic_flag_str = NULL;
char *vic_ip_str = NULL;
char *vic_port_str = NULL;

int  chkpt_mserv_cnt = 0;
char *tlc_chkpt_name_str = NULL;
char *tlc_chkpt_mserv_ipport_str[MAX_TLC_MSERV];
char *tlc_chkpt_mserv_ip[MAX_TLC_MSERV];
int  tlc_chkpt_mserv_port[MAX_TLC_MSERV];
char chkpt_recovery_inst_str[MAX_TLC_MSERV][INST_STR_LEN];
uint64_t mserv_base_page_id[MAX_TLC_MSERV];
uint64_t mserv_allocate_pages[MAX_TLC_MSERV];

uint64_t per_mserv_allocate_pages = 0;

int tlc_config(void);

int do_config_tlc(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    int ret;

    chkpt_mserv_cnt = 0;
    ret = tlc_config(); // parse config file
    return ret;
}

extern uint64_t tlc_ram_size;
extern uint64_t tlc_page_size;
extern void tlc_init(void);
extern int mserv_create_connections(void);

int tlc_config(void){    

    FILE *fp = NULL; 
    int finish_scan_config = 0;
    char line[TLC_STR_LEN];
    char paramstr[TLC_STR_LEN];
    char valuestr[TLC_STR_LEN];
    int  i, flag, cnt = 0;
    char *tmp_str;
    char *tmp_ip_str;
    char *tmp_port_str;

    if((fp = fopen(TLC_CONFIG_FILE, "r")) == NULL){
        printf("TLC configuration file unavailable!\n");
        return -1;
    }

    tlc_init(); // get ram_size, page_size, dirty_size values of vm 
    while(!finish_scan_config){
        memset(paramstr, 0, TLC_STR_LEN);
        memset(valuestr, 0, TLC_STR_LEN);
        // read at most TLC_STR_LEN characters
        tmp_str = fgets(line, TLC_STR_LEN, fp);
        if(tmp_str == NULL){
            printf("config_tlc: EOF reached before end statement!");
            fclose(fp);
            return 0;
        }
        sscanf(line, "%s %s",paramstr, valuestr);

//printf(" read %s %s len = %d size = %d\n", paramstr, valuestr, strlen(valuestr), sizeof(valuestr));

        if(strncmp(paramstr,"TLC_MSERV_IP", strlen("TLC_MSERV_IP")) == 0){
            tlc_mserv_ip_str = calloc(TLC_STR_LEN, 1);
            memcpy(tlc_mserv_ip_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, tlc_mserv_ip_str);
        }
        else if(strncmp(paramstr,"TLC_MSERV_BASE_PORT", strlen("TLC_MSERV_BASE_PORT")) == 0){
            tlc_mserv_base_port_str = calloc(TLC_STR_LEN, 1);
            memcpy(tlc_mserv_base_port_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, tlc_mserv_base_port_str);
        }
        else if(strncmp(paramstr,"TLC_CHKPT_NAME", strlen("TLC_CHKPT_NAME")) == 0){
            tlc_chkpt_name_str = calloc(TLC_STR_LEN, 1);
            memcpy(tlc_chkpt_name_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, tlc_chkpt_name_str);
        }
        else if(strncmp(paramstr,"TLC_CHKPT_MSERV_IPPORT", strlen("TLC_CHKPT_MSERV_IPPORT")) == 0){
          if(chkpt_mserv_cnt < MAX_TLC_MSERV){
            tlc_chkpt_mserv_ipport_str[chkpt_mserv_cnt] = calloc(TLC_STR_LEN, 1);
            memcpy(tlc_chkpt_mserv_ipport_str[chkpt_mserv_cnt], valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, tlc_chkpt_mserv_ipport_str[chkpt_mserv_cnt]);
            chkpt_mserv_cnt++;
          }
          else{
            printf("tlc_conf error: Number of checkpoint memory server exceeds MAX_TLC_MSERV %d\n", MAX_TLC_MSERV);
          }
        }
        else if(strncmp(paramstr,"TLC_INTERVAL", strlen("TLC_INTERVAL")) == 0){
            tlc_interval_str = calloc(TLC_STR_LEN, 1);
            memcpy(tlc_interval_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, tlc_interval_str);
        }
        else if(strncmp(paramstr,"TLC_SLOWDOWN_INTERVAL", strlen("TLC_SLOWDOWN_INTERVAL")) == 0){
            tlc_slowdown_interval_str = calloc(TLC_STR_LEN, 1);
            memcpy(tlc_slowdown_interval_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, tlc_slowdown_interval_str);
        }
        else if(strncmp(paramstr,"TLC_SLOWDOWN_TRIGGER", strlen("TLC_SLOWDOWN_TRIGGER")) == 0){
            tlc_slowdown_trigger_str = calloc(TLC_STR_LEN, 1);
            memcpy(tlc_slowdown_trigger_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, tlc_slowdown_trigger_str);
        }
        else if(strncmp(paramstr,"TLC_SLOWDOWN_STEPWISE", strlen("TLC_SLOWDOWN_STEPWISE")) == 0){
            tlc_slowdown_stepwise_str = calloc(TLC_STR_LEN, 1);
            memcpy(tlc_slowdown_stepwise_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, tlc_slowdown_stepwise_str);
        }
        else if(strncmp(paramstr,"TLC_SLOWDOWN_AGGRESSIVENESS", strlen("TLC_SLOWDOWN_AGGRESSIVENESS")) == 0){
            tlc_slowdown_aggressiveness_str = calloc(TLC_STR_LEN, 1);
            memcpy(tlc_slowdown_aggressiveness_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, tlc_slowdown_aggressiveness_str);
        }
        else if(strncmp(paramstr,"TLC_SLOWDOWN_MARKER", strlen("TLC_SLOWDOWN_MARKER")) == 0){
            tlc_slowdown_marker_str = calloc(TLC_STR_LEN, 1);
            memcpy(tlc_slowdown_marker_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, tlc_slowdown_marker_str);
        }
        else if(strncmp(paramstr,"VIC_FLAG", strlen("VIC_FLAG")) == 0){
            vic_flag_str = calloc(TLC_STR_LEN, 1);
            memcpy(vic_flag_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, vic_flag_str);
        }
        else if(strncmp(paramstr,"VIC_IP", strlen("VIC_IP")) == 0){
            vic_ip_str = calloc(TLC_STR_LEN, 1);
            memcpy(vic_ip_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, vic_ip_str);
        }
        else if(strncmp(paramstr,"VIC_PORT", strlen("VIC_PORT")) == 0){
            vic_port_str = calloc(TLC_STR_LEN, 1);
            memcpy(vic_port_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, vic_port_str);
        }
        else if(strncmp(paramstr,"TLC_END", strlen("TLC_END")) == 0){
            finish_scan_config = 1;
printf(" tlc_conf: %s = %s \n", paramstr, valuestr);
        }
    }

    if((tlc_chkpt_name_str != NULL)&&(chkpt_mserv_cnt > 0)){
      per_mserv_allocate_pages = tlc_page_size/chkpt_mserv_cnt;
printf(" in mserv create conn per_alloc_pages = %" PRId64 "\n", per_mserv_allocate_pages); 

      for(i = 0; i < chkpt_mserv_cnt; i++){
        char *tmp_ipport_str = calloc(TLC_STR_LEN, 1);
        memcpy(tmp_ipport_str, tlc_chkpt_mserv_ipport_str[i] ,strlen(tlc_chkpt_mserv_ipport_str[i])); 
        // split tlc_chkpt_mserv_ipport_str
        if((tmp_ip_str = strtok(tmp_ipport_str, ":")) == NULL){
            printf("scanning chkpt IP error on %d\n", i);
            fflush(stdout);
            break;
        }
        if((tmp_port_str = strtok(NULL, ":")) == NULL){
            printf("scanning chkpt port error on %d\n", i);
            fflush(stdout);
            break;
        }
        if(tlc_chkpt_mserv_ip[i] == NULL) 
            tlc_chkpt_mserv_ip[i] = malloc(TLC_STR_LEN);
        memset(tlc_chkpt_mserv_ip[i], 0, TLC_STR_LEN);

printf(" mserv_ipport = %s tmp_ipport_str %s tmp_ip_str = %s tmp_port_str = %s ipstrlen = %d\n", 
tlc_chkpt_mserv_ipport_str[i], tmp_ipport_str, tmp_ip_str, tmp_port_str, strlen(tmp_ip_str));
printf(" tmp_ip %x and tmp_ipport %x and diff %d\n", (void *)tmp_ip_str, (void *)tmp_ipport_str, (tmp_ipport_str - tmp_ip_str));
fflush(stdout);

        memcpy(tlc_chkpt_mserv_ip[i], tmp_ip_str, strlen(tmp_ip_str));
        tlc_chkpt_mserv_port[i] = atoi(tmp_port_str);

        // create recovery string 
        // Calculate number of pages to be allocated on the server 
        // Use Block distribution 
        mserv_base_page_id[i] = i*per_mserv_allocate_pages;
        if((i+1 == chkpt_mserv_cnt) && ((i+1)*per_mserv_allocate_pages < tlc_page_size)){
          mserv_allocate_pages[i] = tlc_page_size - mserv_base_page_id[i];
        }
        else{
          mserv_allocate_pages[i] = per_mserv_allocate_pages;
        }

        memset(chkpt_recovery_inst_str[i], 0, INST_STR_LEN);
        snprintf(
              chkpt_recovery_inst_str[i], INST_STR_LEN,
              "%s:%" PRId64 ":%" PRId64 ":%s",
              tlc_chkpt_mserv_ipport_str[i],
              mserv_base_page_id[i], 
              mserv_allocate_pages[i], 
              tlc_chkpt_name_str);
 
printf("tlc_config: chkpt name = %s recover instr = %s\n", tlc_chkpt_name_str, chkpt_recovery_inst_str[i]);

        free(tmp_ipport_str);
      }

      if((tlc_chkpt_state_flag = mserv_create_connections()) == TLC_CHKPT_ABORT){
        printf("tlc_config: cannot establish connection to mem servers\n");
        return -1;
      }
    }
    return 0;
}

int do_migrate_set_speed(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    int64_t d;
    MigrationState *s;

    d = qdict_get_int(qdict, "value");
    if (d < 0) {
        d = 0;
    }

    s = migrate_get_current();
    s->bandwidth_limit = d;
    qemu_file_set_rate_limit(s->file, s->bandwidth_limit);

    return 0;
}

int do_migrate_set_downtime(Monitor *mon, const QDict *qdict,
                            QObject **ret_data)
{
    double d;

    d = qdict_get_double(qdict, "value") * 1e9;
    d = MAX(0, MIN(UINT64_MAX, d));
    max_downtime = (uint64_t)d;

    return 0;
}
