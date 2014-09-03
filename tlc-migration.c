/*
 * TLC
 *
 * Copyright Vasabilab. 2012
 *
 * Authors:
 *  Kasidit Chanchio   <kasiditchanchio@gmail.com>
 *
 *
 */

#include "qemu-common.h"
#include "migration.h"
#include "monitor.h"
//#include "qemu-timer.h"
#include "buffered_file.h"
#include "sysemu.h"
//#include "block.h"
//#include "qemu_socket.h"
#include "block-migration.h"
//#include "qmp-commands.h"

#include "tlc.h"

// TLC declarations
#include 	<pthread.h>
#include 	"tlc-debug.h"

enum {
    MIG_STATE_ERROR,
    MIG_STATE_SETUP,
    MIG_STATE_CANCELLED,
    MIG_STATE_ACTIVE,
    MIG_STATE_COMPLETED,
};

extern NotifierList migration_state_notifiers;

int64_t 	mig_start, mig_finish;
int64_t 	mig_period_start, mig_period_end, 
		mig_duration, acc_vm_overhead, max_mig_duration;
int 		duration_id;

int64_t 	stage_start, stage_finish;

int64_t 	epoch_start, epoch_end, epoch_duration;
uint64_t 	pages_copied_last_epoch; 
uint64_t 	pages_gen_last_epoch;

extern uint64_t tlc_s21_pages_transferred;
extern uint64_t tlc_s21_bytes_transferred;
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

extern uint64_t tlc_s22_pages_transferred;
extern uint64_t tlc_s22_bytes_transferred;

// TLC: begin
extern void tlc_migration_init(int flag);
extern void tlc_migration_finish(int flag);
extern void *page_saver_t(void *args);

#define MAX_THROTTLE  (32 << 20)      /* Migration speed throttling */

//MigrationState *tlc_migrate_get_current(void);

static MigrationState *tlc_migrate_get_current(void)
{
    static MigrationState current_migration = {
        .state = MIG_STATE_SETUP,
        .bandwidth_limit = MAX_THROTTLE,
    };

    return &current_migration;
}

int final_migration_cleanup(MigrationState *s);
/*
// TLC: end
*/
extern int vic_flag;
extern int vic_report(char *message);

int state_transfer_type = NOTYPE; 
int state_transfer_mode = TLM_NO_MODE; 
int chkpt_mserv_cnt = 0;

int tlc_config(char *filename);

static int get_str_sep(char *buf, int buf_size, const char **pp, int sep)
{
    const char *p, *p1;
    int len;
    p = *pp;
    p1 = strchr(p, sep);
    if (!p1)
        return -1;
    len = p1 - p;
    p1++;
    if (buf_size > 0) {
        if (len > buf_size - 1)
            len = buf_size - 1;
        memcpy(buf, p, len);
        buf[len] = '\0';
    }
    *pp = p1;
    return 0;
}

static int get_str_sep_unchange_pp(char *buf, int buf_size, const char **pp, int sep)
{
    const char *p, *p1;
    int len;
    p = *pp;
    p1 = strchr(p, sep);

    if (!p1)
       return -1;
    len = p1 - p;
    if (len == 0)
       return 1;

    p1++;
    if (buf_size > 0) {
        if (len > buf_size - 1)
            len = buf_size - 1;
        memcpy(buf, p, len);
        buf[len] = '\0';
    }
    //*pp = p1;
    return 0;
}

int64_t tlc_restore_start, tlc_restore_finish;

int tlc_config_builtin_params(char *dest);    

int tlc_qemu_start_incoming_migration(const char *uri);

int tlc_qemu_start_incoming_migration(const char *uri)
{
    const char *default_conf_filename = TLC_CONFIG_FILE;
    char tlc_conf_filename[512];
    char tlc_dest_host[512];
    const char *p_dest = NULL;
    const char *pconf;
    const char *p;
    int ret, ret2;

    state_transfer_type = NOTYPE; 

    tlc_restore_start = qemu_get_clock_ms(rt_clock);

    // on destination
    if (strstart(uri, "tcp:", &pconf)) {
        p = pconf;
        memset(tlc_dest_host, 0, 512);

        if((ret2 = get_str_sep_unchange_pp(tlc_dest_host, sizeof(tlc_dest_host), &p, ':')) < 0){
          printf("unknown tcp mig destination: %s\n", tlc_dest_host);
          return -EINVAL;
        }
        else if (ret2 == 1){
          p_dest = NULL;
        }
        else if (ret2 == 0){
          p_dest = tlc_dest_host;
        }

        chkpt_mserv_cnt = 0;
        if(tlc_config_builtin_params((char *) p_dest)< 0){
          printf("error conf tcp mig destination: %s\n", tlc_dest_host);
          return -EINVAL;
        }
        state_transfer_type = TLM;
        autostart = 1; 
        ret = tlc_tcp_start_incoming_migration(p);
    } 
#if !defined(WIN32)
    else if (strstart(uri, "tlc-exec:", &p)){ 
        chkpt_mserv_cnt = 0;
        tlc_config((char *) default_conf_filename);
        state_transfer_type = TLC_EXEC;
        autostart = 1;
        ret = tlc_exec_start_incoming_migration(p);
    }
    else if (strstart(uri, "tlcconf:", &pconf)) {
        p = pconf;
        memset(tlc_conf_filename, 0, 512);

        if(get_str_sep(tlc_conf_filename, sizeof(tlc_conf_filename), &p, ':') < 0){
          printf("unknown conf filename: %s\n", tlc_conf_filename);
          return -EINVAL;
        }
        chkpt_mserv_cnt = 0;
        if(tlc_config((char *) tlc_conf_filename)< 0){
          printf("error conf filename: %s\n", tlc_conf_filename);
          return -EINVAL;
        }
        state_transfer_type = TLC_EXEC;
        autostart = 1; 
        ret = tlc_exec_start_incoming_migration(p);
    } 
    else if (strstart(uri, "tlc-tcp-standby-dst:", &p)){
        tlc_config((char *) default_conf_filename);
        state_transfer_type = TLC_TCP_STANDBY_DST;
        autostart = 0; 
        ret = tlc_tcp_start_incoming_migration(p);
    }
    else if (strstart(uri, "tlm:", &p)){
printf("in tlm: \n");
fflush(stdout);
        state_transfer_type = TLM;
        tlc_config((char *) default_conf_filename);
printf("in tlm: after tlc_config\n");
fflush(stdout);
        autostart = 1; 
        ret = tlc_tcp_start_incoming_migration(p);
printf("in tlm: after tlc_tcp_start_income\n");
fflush(stdout);
    } 
    else if (strstart(uri, "tlm-exec:", &p)){
printf("in tlm-exec: \n");
fflush(stdout);
        state_transfer_type = TLM_EXEC;
        tlc_config((char *) default_conf_filename);
printf("in tlm-exec: after tlc_config\n");
fflush(stdout);
        autostart = 1; 
        ret = tlc_tcp_start_incoming_migration(p);
printf("in tlm-exec: after tlc_tcp_start_income\n");
fflush(stdout);
    } 
    else if (strstart(uri, "tlmconf:", &pconf)) {
        p = pconf;
        memset(tlc_conf_filename, 0, 512);

        if(get_str_sep(tlc_conf_filename, sizeof(tlc_conf_filename), &p, ':') < 0){
          printf("unknown conf filename: %s\n", tlc_conf_filename);
          return -EINVAL;
        }
        chkpt_mserv_cnt = 0;
        if(tlc_config((char *) tlc_conf_filename)< 0){
          printf("error conf filename: %s\n", tlc_conf_filename);
          return -EINVAL;
        }
        state_transfer_type = TLM;
        autostart = 1; 
        ret = tlc_tcp_start_incoming_migration(p);
    } 
    else if (strstart(uri, "tlmc:", &p)){
        state_transfer_type = TLMC;
        tlc_config((char *) default_conf_filename);
        //autostart = 0;
        autostart = 1; // temporary.. testing
        ret = tlc_tcp_start_incoming_migration(p);
    }
    else if (strstart(uri, "tlmc-standby-dst:", &p)){
        state_transfer_type = TLMC_STANDBY_DST;
        tlc_config((char *) default_conf_filename);
        autostart = 0;
        //autostart = 1; 
        ret = tlc_tcp_start_incoming_migration(p);
    }
    else if (strstart(uri, "tlm-standby-dst:", &p)){
        state_transfer_type = TLM_STANDBY_DST;
        tlc_config((char *) default_conf_filename);
        autostart = 0; 
        ret = tlc_tcp_start_incoming_migration(p);
    }
    else if (strstart(uri, "tlm-standby-srcdst:", &p)){
        state_transfer_type = TLM_STANDBY_SRCDST;
        tlc_config((char *) default_conf_filename);
        autostart = 0; 
        ret = tlc_tcp_start_incoming_migration(p);
    }
    else if (strstart(uri, "exec:", &p)){
        ret = tlc_exec_start_incoming_migration(p);
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

void tlc_process_incoming_migration(QEMUFile *f);

void tlc_process_incoming_migration(QEMUFile *f)
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

    // Make sure all file formats flush their mutable metadata 
    bdrv_invalidate_cache_all();

    if (autostart) {

        tlc_restore_finish = qemu_get_clock_ms(rt_clock);
	printf("*Restore time: %" PRId64 " ms\n", tlc_restore_finish - tlc_restore_start);
	fflush(stdout);
        vm_start();
    } else {
        runstate_set(RUN_STATE_PRELAUNCH);
    }
}

MigrationInfo *tlc_qmp_query_migrate(Error **errp);

MigrationInfo *tlc_qmp_query_migrate(Error **errp)
{
    MigrationInfo *info = g_malloc0(sizeof(*info));
    MigrationState *s = tlc_migrate_get_current();

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

static void tlc_migrate_fd_monitor_suspend(MigrationState *s, Monitor *mon)
{
    if (monitor_suspend(mon) == 0) {
	
	DREG{printf("suspending monitor\n");
	fflush(stdout);}
    } else {
        monitor_printf(mon, "terminal does not allow synchronous "
                       "migration, continuing detached\n");
    }
}

static int tlc_migrate_fd_cleanup(MigrationState *s)
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

void tlc_migrate_fd_error(MigrationState *s);

void tlc_migrate_fd_error(MigrationState *s)
{
    s->state = MIG_STATE_ERROR;
    notifier_list_notify(&migration_state_notifiers, s);
    tlc_migrate_fd_cleanup(s);
}

static void tlc_migrate_fd_completed(MigrationState *s)
{
    if (tlc_migrate_fd_cleanup(s) < 0) {
        s->state = MIG_STATE_ERROR;
    } else {
        s->state = MIG_STATE_COMPLETED;
    }

    notifier_list_notify(&migration_state_notifiers, s);
    
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
        
	//DDETAIL{printf("mfputbuffer: set handler mig fd put norify() ret was %d\n", (int)ret);
	//fflush(stdout);}

        //qemu_set_fd_handler2(s->fd, NULL, NULL, migrate_fd_put_notify, s);
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
        	tlc_migrate_fd_error(s);
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
                case TLM_EXEC: 
                case TLC_TCP_STANDBY_DST: 
                case TLM_STANDBY_DST: 
                case TLMC_STANDBY_DST: 
                    vm_start(); // TLC, let the vm continue.
                    break;
                case TLM: 
                case TLMC: 
                case TLM_STANDBY_SRCDST: 
                    // temporary testing
                    //printf("pause source vm!\n");
	            //fflush(stdout);
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

extern int tlc_pages_sent_mserv; 

#define	TLC_OUTGOING_PAGES	1
#define TLC_INCOMING_PAGES	2

int final_migration_cleanup(MigrationState *s)
{

DREG{	
printf("Total pages PS sent to mserv during Stage 2.2 = %d pages\n", tlc_pages_sent_mserv);
printf("Total pages PS sent to dst during Stage 2.2 = %" PRId64 " pages\n", tlc_s22_pages_transferred);
printf("Total bytes PS sent to dst during Stage 2.2 = %" PRId64 " kbytes\n", tlc_s22_bytes_transferred >> 10);
printf("Total pages CHK sent on Stage 2.1 = %" PRIu64 " pages\n", tlc_s21_pages_transferred);
printf("Total bytes CHK sent on Stage 2.1 = %" PRIu64 " kbytes\n", ram_bytes_transferred() >> 10);
}	
	
	tlc_migrate_fd_completed(s); // fflush buffer to file (take time)
	tlc_migration_finish(TLC_OUTGOING_PAGES);
	
        DVIC{
	    char progress_string[VIC_STR_LEN];
            int64_t vic_mig_thread_finish = qemu_get_clock_ms(rt_clock);

            if(vic_flag){
                sprintf(progress_string, "s4 %d %" PRId64 " %" PRIu64 " ", tlc_pages_sent_mserv, tlc_s21_pages_transferred, (vic_mig_thread_finish - mig_start));
	        vic_report(progress_string);
            }
            printf("s4 %d %" PRId64 " %" PRIu64 "\n", tlc_pages_sent_mserv, tlc_s21_pages_transferred, (vic_mig_thread_finish - mig_start));
        }
        
	tlc_s21_bytes_transferred = 0;
	tlc_s21_pages_transferred = 0;

	mthread = 0;
	return 0;
}

extern int 	tlc_interval;
extern int 	tlc_slowdown_interval;
extern int 	tlc_freeze_interval;
extern int 	min_exe_burst;

extern 		int 	slowdown_aggressiveness; 
extern 		double	slowdown_progress_unit;
extern 		int	stepwise_reduction_counter;
extern 		int 	trigger_slowdown;
extern 		double 	slowdown_marker;

extern 		int stepwise_slowdown;
//extern 		int smp_cpus; 

extern void save_vcpu_affinity(void);
int slowdown_cpu(int reduced_num_cpus);

//TLC profiling
extern tlc_profile_type *tlc_profile;

int64_t s2_end_time = 0;

extern int nodirty; 
extern pthread_cond_t  have_dirty;
extern pthread_mutex_t mutex_save_page;
extern int qemu_save_eof(QEMUFile *f);

void migrate_fd_connect(MigrationState *s)
{
    int ret;
    
    s->state = MIG_STATE_ACTIVE;
    s->file = tlc_qemu_fopen_ops_buffered(s,
                                      s->bandwidth_limit,
                                      migrate_fd_put_buffer,
                                      migrate_fd_put_ready,
                                      migrate_fd_wait_for_unfreeze,
                                      migrate_fd_close);

    ret = qemu_savevm_state_begin(s->mon, s->file, s->blk, s->shared);
    if (ret < 0) {
        DREG{printf("mfc: qssb failed, %d\n", ret);}
        tlc_migrate_fd_error(s);
        return;
    }

    //if((trigger_slowdown == 1) && (slowdown_marker == 0)){
//	force_slowdown();
    //}

    pthread_mutex_unlock(&vm_waitfor_checkpoint_thread);
    
    migrate_fd_put_ready(s);
    
    pthread_mutex_lock(&mutex_save_page);
    idone = 1; // use this to tell the PS thread that chkp thr finishes stage2
    s2_end_time = qemu_get_clock_ms(rt_clock);
    nodirty = 0;
    pthread_cond_signal(&have_dirty); // in case the PS is waiting
    pthread_mutex_unlock(&mutex_save_page);

    pthread_mutex_lock(&mutex_idone);
    while (save_dev_state == 0)
    	pthread_cond_wait(&save_dev_state_done, &mutex_idone);
	// Wait until stage 3 on the vm thread is done
    pthread_mutex_unlock(&mutex_idone); 
    
    stage_start = qemu_get_clock_ms(rt_clock);
    qemu_save_eof(s->file);
    stage_finish = qemu_get_clock_ms(rt_clock);	

    if(s3_mig_trans_failure){
        printf("migrate_fd_connect: io_thread: Stage 4 error s3_failure is set\n"); 
    }
    else{
        DREG{
        printf("Elasped time from start to completion of Stage 4: %" PRId64 " ms\n", 
	    stage_finish - mig_start);}
    }

    final_migration_cleanup(s); // this will also fflush contents
    
}

MigrationState *migrate_init(Monitor *mon, int detach, int blk, int inc);

MigrationState *migrate_init(Monitor *mon, int detach, int blk, int inc)
{
    MigrationState *s = tlc_migrate_get_current();
    int64_t bandwidth_limit = s->bandwidth_limit;

    memset(s, 0, sizeof(*s));
    s->bandwidth_limit = bandwidth_limit;
    s->blk = blk;
    s->shared = inc;

    s->mon = mon;
    s->bandwidth_limit = bandwidth_limit;
    s->state = MIG_STATE_SETUP;

    if (!detach) {
        tlc_migrate_fd_monitor_suspend(s, mon);
    }
    return s;
}

static void local_tlc_migrate_fd_cancel(MigrationState *s)
{
    if (s->state != MIG_STATE_ACTIVE)
        return;

    s->state = MIG_STATE_CANCELLED;
    notifier_list_notify(&migration_state_notifiers, s);
    qemu_savevm_state_cancel(s->mon, s->file);

    tlc_migrate_fd_cleanup(s);
}

void tlc_do_migrate_cancel(void);

void tlc_do_migrate_cancel(void)
{
    
    if(mthread){
        printf("Error: (migrate_cancel) previous migration is in prograss!\n");
        fflush(stdout);
	return;
    }

    local_tlc_migrate_fd_cancel(tlc_migrate_get_current());
}

void tlc_do_migrate_set_speed(int64_t d);

// We use this to set slowdown aggressiveness
void tlc_do_migrate_set_speed(int64_t d)
{
    //MigrationState *s;

    slowdown_aggressiveness = 2; // hard code for now
printf(" slowdown aggressiveness = %d\n", slowdown_aggressiveness);
    slowdown_cpu(slowdown_aggressiveness);
    //if((trigger_slowdown == 1) && (slowdown_marker == 0)){
//	force_slowdown();
 //       trigger_slowdown = 0;
  //  }

    //s = tlc_migrate_get_current();
    //s->bandwidth_limit = d;
    //qemu_file_set_rate_limit(s->file, s->bandwidth_limit);
}

void tlc_do_migrate_set_downtime(int64_t d);

void tlc_do_migrate_set_downtime(int64_t d)
{
    if(mthread){
        printf("Error: (migrate_set_downtime) previous migration is in prograss!\n");
        fflush(stdout);
	return;
    }
}

// Global vars to pass values to do_migrate_t
typedef struct {
    MigrationState *s;
    Monitor *mon;
    int detach;
    const char *p;
} Tmig;

Tmig t;

extern uint64_t mserv_set_item_cnt; // debugging
extern uint64_t base_mserv_set_item_cnt; // debugging

int  tlc_chkpt_status = TLC_CHKPT_INIT;

extern int mserv_create_connections(void);

static int do_migrate_state(MigrationState *s, Monitor *mon, int detach, const char *p)
{
    int ret = 0;
    mserv_set_item_cnt = 0;
    base_mserv_set_item_cnt = 0; 
    // TLC state transfer
    if (state_transfer_type == TLC_EXEC){
        if((tlc_chkpt_status = mserv_create_connections()) == TLC_CHKPT_ABORT){
          printf("do_mig_state: %d cannot establish connection to mem servers\n", state_transfer_type);
          goto mig_state_out;
        }
        ret = tlc_exec_start_outgoing_migration(s, p);
    } else if (state_transfer_type == TLC_TCP_STANDBY_DST) {
        if((tlc_chkpt_status = mserv_create_connections()) == TLC_CHKPT_ABORT){
          printf("do_mig_state: %d cannot establish connection to mem servers\n", state_transfer_type);
          goto mig_state_out;
        }
        ret = tlc_tcp_start_outgoing_migration(s, p);
    // TLM state transfer
    } else if (state_transfer_type == TLM){
        if((tlc_chkpt_status = mserv_create_connections()) == TLC_CHKPT_ABORT){
          printf("do_mig_state: %d cannot establish connection to mem servers\n", state_transfer_type);
          goto mig_state_out;
        }
        ret = tlc_tcp_start_outgoing_migration(s, p);
    // TLM_EXEC state transfer
    } else if (state_transfer_type == TLM_EXEC){
        if((tlc_chkpt_status = mserv_create_connections()) == TLC_CHKPT_ABORT){
          printf("do_mig_state: %d cannot establish connection to mem servers\n", state_transfer_type);
          goto mig_state_out;
        }
        ret = tlc_tcp_start_outgoing_migration(s, p);
    } else if (state_transfer_type == TLMC){
        if((tlc_chkpt_status = mserv_create_connections()) == TLC_CHKPT_ABORT){
          printf("do_mig_state: %d cannot establish connection to mem servers\n", state_transfer_type);
          goto mig_state_out;
        }
        ret = tlc_tcp_start_outgoing_migration(s, p);
    } else if (state_transfer_type == TLMC_STANDBY_DST){
        if((tlc_chkpt_status = mserv_create_connections()) == TLC_CHKPT_ABORT){
          printf("do_mig_state: %d cannot establish connection to mem servers\n", state_transfer_type);
          goto mig_state_out;
        }
        ret = tlc_tcp_start_outgoing_migration(s, p);
    } else if (state_transfer_type == TLM_STANDBY_DST){
        if((tlc_chkpt_status = mserv_create_connections()) == TLC_CHKPT_ABORT){
          printf("do_mig_state: %d cannot establish connection to mem servers\n", state_transfer_type);
          goto mig_state_out;
        }
        ret = tlc_tcp_start_outgoing_migration(s, p);
    } else if (state_transfer_type == TLM_STANDBY_SRCDST){
        if((tlc_chkpt_status = mserv_create_connections()) == TLC_CHKPT_ABORT){
          printf("do_mig_state: %d cannot establish connection to mem servers\n", state_transfer_type);
          goto mig_state_out;
        }
        ret = tlc_tcp_start_outgoing_migration(s, p);
    } else {
        DREG{printf("unknown protocol info: %s\n", p);}
        ret  = -EINVAL;
    }

mig_state_out: 
    if (ret < 0) {
        printf("migration failed: %d\n", ret);
        local_tlc_migrate_fd_cancel(s);
    }
    return ret;
}

void *do_migrate_t(void *arg);

void *do_migrate_t(void *arg)
{
    int64_t mig_thread_start, mig_thread_finish;
    
    chkp_thread_id = pthread_self(); // for use in mfpr 
    
    mig_thread_start = qemu_get_clock_ms(rt_clock);
    do_migrate_state(t.s, t.mon, t.detach, t.p);
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

// TLC migration configuration parameter strings

char *tlc_mserv_ip_str = NULL;
char *tlc_mserv_base_port_str = NULL;
char *tlc_interval_str = NULL;
char *tlc_slowdown_interval_str = NULL;
char *tlc_slowdown_trigger_str = NULL;
char *tlc_freeze_flag_str = NULL;
char *tlc_freeze_interval_str = NULL;
char *tlc_freeze_minexeburst_str = NULL;
char *tlc_slowdown_stepwise_str = NULL;
char *tlc_slowdown_aggressiveness_str = NULL;
char *tlc_slowdown_marker_str = NULL;
char *vic_flag_str = NULL;
char *vic_ip_str = NULL;
char *vic_port_str = NULL;

char *tlc_chkpt_name_str = NULL;
char *tlc_chkpt_type_str = NULL;
char *tlc_chkpt_mserv_ipport_str[MAX_TLC_MSERV];
char *tlc_chkpt_mserv_ip[MAX_TLC_MSERV];
int  tlc_chkpt_mserv_port[MAX_TLC_MSERV];
char chkpt_recovery_inst_str[MAX_TLC_MSERV][INST_STR_LEN];
uint64_t mserv_base_page_id[MAX_TLC_MSERV];
uint64_t mserv_allocate_pages[MAX_TLC_MSERV];

uint64_t per_mserv_allocate_pages = 0;

extern int 	tlc_mserv_base_port;
extern char 	*tlc_mserv_ip;

//extern int 	tlc_interval;
//extern int 	tlc_slowdown_interval;
extern int      tlc_chkpt_type_flag;

extern int 	vic_port;
extern char 	*vic_ip;

extern uint64_t tlc_ram_size;
extern uint64_t tlc_page_size;

extern void tlc_init(void);
extern int vic_create_connection(int num_conn);


int tlc_do_migrate(Monitor *mon, int detach, int blk, int inc, const char *uri);

int tlc_do_migrate(Monitor *mon, int detach, int blk, int inc, const char *uri)
{
    char filename[512];
    char tlc_dest_host[512];
    const char *pconf;
    const char *p;

    if(mthread){
        printf("Error: previous migration is in prograss!\n");
        fflush(stdout);
	return -1;
    }

    state_transfer_type = NOTYPE; 
    state_transfer_mode = TLM_NO_MODE;

    // TLC state transfer
    if (strstart(uri, "tlc-exec:", &p)) {
        state_transfer_type = TLC_EXEC;
    } else if (strstart(uri, "tlc-tcp-standby-dst:", &p)) {
        state_transfer_type = TLC_TCP_STANDBY_DST;
    } else if (strstart(uri, "tcp:", &pconf)) {
        p = pconf;
        memset(tlc_dest_host, 0, 512);

        if(get_str_sep_unchange_pp(tlc_dest_host, sizeof(tlc_dest_host), &p, ':') < 0){
          printf("unknown tcp mig destination: %s\n", tlc_dest_host);
          return -EINVAL;
        }
        chkpt_mserv_cnt = 0;
        if(tlc_config_builtin_params((char *) tlc_dest_host)< 0){
          printf("error conf tcp mig destination: %s\n", tlc_dest_host);
          return -EINVAL;
        }
        state_transfer_type = TLM;
    } else if (strstart(uri, "tlm:", &p)) {
        state_transfer_type = TLM;
    } else if (strstart(uri, "tlm-exec:", &p)) {
        state_transfer_type = TLM_EXEC;
    } else if (strstart(uri, "tlmone:", &p)) {
        state_transfer_mode = TLM_ONEPASS_MODE;
        state_transfer_type = TLM;
    } else if (strstart(uri, "tlmconf:", &pconf)) {
        p = pconf;
        if(get_str_sep(filename, sizeof(filename), &p, ':') < 0){
          DREG{printf("unknown conf filename: %s\n", filename);}
          return -EINVAL;
        }
        chkpt_mserv_cnt = 0;
        if(tlc_config((char *) filename)< 0){
          DREG{printf("error conf filename: %s\n", filename);}
          return -EINVAL;
        }
        state_transfer_type = TLM;
    } else if (strstart(uri, "tlmc:", &p)) { // temporary
        state_transfer_type = TLMC;
    } else if (strstart(uri, "tlmc-standby-dst:", &p)) {
        state_transfer_type = TLMC_STANDBY_DST;
    } else if (strstart(uri, "tlm-standby-dst:", &p)) {
        state_transfer_type = TLM_STANDBY_DST;
    } else if (strstart(uri, "tlm-standby-srcdst:", &p)) {
        state_transfer_type = TLM_STANDBY_SRCDST;
    } else if (strstart(uri, "conf:", &p)) {
        chkpt_mserv_cnt = 0;
        return tlc_config((char *) p);
    } else {
        DREG{printf("unknown migration protocol: %s\n", uri);}
        return -EINVAL;
    }

    MigrationState *s = tlc_migrate_get_current();
    
    ncpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    
    if (s->state == MIG_STATE_ACTIVE) {
        monitor_printf(mon, "migration already in progress\n");
        return -1;
    }

    s = migrate_init(mon, detach, blk, inc);   

    live_storage_init(TLC_OUTGOING_PAGES, 6); // We can do this before migration (timing).

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

    tlc_pages_sent_mserv = 0;
    tlc_s21_bytes_transferred = 0;
    tlc_s21_pages_transferred = 0;
    tlc_s22_pages_transferred = 0;
    tlc_s22_bytes_transferred = 0;

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
        t.p = p;
        
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

int tlc_config(char *filename){    

    FILE *fp = NULL; 
    int finish_scan_config = 0;
    char line[TLC_STR_LEN];
    char paramstr[TLC_STR_LEN];
    char valuestr[TLC_STR_LEN];
    int  i;
    char *tmp_str;
    char *tmp_ip_str;
    char *tmp_port_str;
    memset(mserv_allocate_pages, 0, MAX_TLC_MSERV*sizeof(uint64_t));

    if((fp = fopen(filename, "r")) == NULL){
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
        else if(strncmp(paramstr,"TLC_CHKPT_TYPE", strlen("TLC_CHKPT_TYPE")) == 0){
            tlc_chkpt_type_str = calloc(TLC_STR_LEN, 1);
            memcpy(tlc_chkpt_type_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, tlc_chkpt_type_str);
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
        else if(strncmp(paramstr,"TLC_FREEZE_INTERVAL", strlen("TLC_FREEZE_INTERVAL")) == 0){ // TLC FRZ
            tlc_freeze_interval_str = calloc(TLC_STR_LEN, 1);
            memcpy(tlc_freeze_interval_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, tlc_freeze_interval_str);
        }
        else if(strncmp(paramstr,"TLC_FREEZE_MINEXEBURST", strlen("TLC_FREEZE_MINEXEBURST")) == 0){ // TLC FRZ
            tlc_freeze_minexeburst_str = calloc(TLC_STR_LEN, 1);
            memcpy(tlc_freeze_minexeburst_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, tlc_freeze_minexeburst_str);
        }
        else if(strncmp(paramstr,"TLC_FREEZE_FLAG", strlen("TLC_FREEZE_FLAG")) == 0){
            tlc_freeze_flag_str = calloc(TLC_STR_LEN, 1);
            memcpy(tlc_freeze_flag_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, tlc_freeze_flag_str);
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

    // Processing given string values 

    if (tlc_mserv_base_port_str!= NULL){
	tlc_mserv_base_port = atoi(tlc_mserv_base_port_str);
    }

    if(tlc_mserv_ip_str != NULL){
	tlc_mserv_ip = tlc_mserv_ip_str;
    }

    if (tlc_interval_str!= NULL){
	tlc_interval = atoi(tlc_interval_str);
    }

    if (tlc_slowdown_interval_str != NULL){
	tlc_slowdown_interval = atoi(tlc_slowdown_interval_str);
    }

    if (tlc_freeze_interval_str != NULL){
	tlc_freeze_interval = atoi(tlc_freeze_interval_str);
printf("freeze interval = %d\n", tlc_freeze_interval);
    }

    if (tlc_freeze_minexeburst_str != NULL){
	min_exe_burst = atoi(tlc_freeze_minexeburst_str);
printf("min exe burst = %d\n", min_exe_burst);
    }

    if (vic_flag_str!= NULL){
        if(strncmp(vic_flag_str, "OFF", 3) == 0)
    	  vic_flag = 0;
	else
    	  vic_flag = 1;
    }
    else vic_flag = 0;

    if (vic_port_str!= NULL){
	vic_port = atoi(vic_port_str);
    }

    if(vic_ip_str != NULL)
	vic_ip = vic_ip_str;
//printf("vic_flag = %d VIC PORT = %d VIC IP=%s \n", vic_flag, vic_port, vic_ip);

    if(vic_flag) vic_create_connection(1); // VIC connection

    if((tlc_chkpt_name_str != NULL)&&(chkpt_mserv_cnt > 0)){

      if (tlc_chkpt_type_str!= NULL){
        if(strncmp(tlc_chkpt_type_str, "CYCLIC_DC", 9) == 0){
    	  tlc_chkpt_type_flag = TLC_MEM_ADDR_CYCLIC_DOUBLE_CHANNELS;
        }
        else if(strncmp(tlc_chkpt_type_str, "BLOCK_DC", 8) == 0){
    	  tlc_chkpt_type_flag = TLC_MEM_ADDR_BLOCK_DOUBLE_CHANNELS;
        }
        else if(strncmp(tlc_chkpt_type_str, "CYCLIC", 6) == 0){
    	  tlc_chkpt_type_flag = TLC_MEM_ADDR_CYCLIC;
        }
	else if(strncmp(tlc_chkpt_type_str, "BLOCK", 5) == 0){
    	  tlc_chkpt_type_flag = TLC_MEM_ADDR_BLOCK;
        }
      }
      else{
    	tlc_chkpt_type_flag = TLC_MEM_ADDR_CYCLIC;
      }

      // Calculate number of pages to be allocated on the server 
      if(
         (tlc_chkpt_type_flag == TLC_MEM_ADDR_BLOCK) ||
         (tlc_chkpt_type_flag == TLC_MEM_ADDR_BLOCK_DOUBLE_CHANNELS)
        ){
        per_mserv_allocate_pages = (tlc_page_size/chkpt_mserv_cnt);
        printf(" BLOCK in mserv create conn per_alloc_pages = %" PRId64 "\n", per_mserv_allocate_pages); 
      }
      else if(
         (tlc_chkpt_type_flag == TLC_MEM_ADDR_CYCLIC) ||
         (tlc_chkpt_type_flag == TLC_MEM_ADDR_CYCLIC_DOUBLE_CHANNELS)
        ){
        per_mserv_allocate_pages = (tlc_page_size/chkpt_mserv_cnt)+1;
        printf(" CYCLIC in mserv create conn per_alloc_pages = %" PRId64 "\n", per_mserv_allocate_pages); 
      }

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
tlc_chkpt_mserv_ipport_str[i], tmp_ipport_str, tmp_ip_str, tmp_port_str, (int) strlen(tmp_ip_str));
//printf(" tmp_ip %x and tmp_ipport %x and diff %d\n", (unsigned int)tmp_ip_str, (unsigned int)tmp_ipport_str, (int) (tmp_ipport_str - tmp_ip_str));
fflush(stdout);

        memcpy(tlc_chkpt_mserv_ip[i], tmp_ip_str, strlen(tmp_ip_str));
        tlc_chkpt_mserv_port[i] = atoi(tmp_port_str);

        // create recovery string 
    	if(
           (tlc_chkpt_type_flag == TLC_MEM_ADDR_BLOCK) ||
           (tlc_chkpt_type_flag == TLC_MEM_ADDR_BLOCK_DOUBLE_CHANNELS) 
          ){
          mserv_base_page_id[i] = i*per_mserv_allocate_pages;
          if((i+1 == chkpt_mserv_cnt) && ((i+1)*per_mserv_allocate_pages < tlc_page_size)){
            mserv_allocate_pages[i] = tlc_page_size - mserv_base_page_id[i];
          }
          else{
            mserv_allocate_pages[i] = per_mserv_allocate_pages;
          }
        }
        else if(
          (tlc_chkpt_type_flag == TLC_MEM_ADDR_CYCLIC) ||
          (tlc_chkpt_type_flag == TLC_MEM_ADDR_CYCLIC_DOUBLE_CHANNELS)
          ){
          mserv_base_page_id[i] = 0;
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
    }
    return 0;
}

int tlc_do_config_tlc(char *filename);

int tlc_do_config_tlc(char *filename){
    int ret;

    if(mthread){
        printf("Error: (do_config) previous migration is in prograss!\n");
        fflush(stdout);
	return -1;
    }

    chkpt_mserv_cnt = 0;
    ret = tlc_config((char *)filename); // parse config file
    return ret;
}

int tlc_config_builtin_params(char *dest){    

    //FILE *fp = NULL; 
    //int finish_scan_config = 0;
    //char line[TLC_STR_LEN];
    char paramstr[TLC_STR_LEN];
    char valuestr[TLC_STR_LEN];
    //int  i;
    //char *tmp_str;
    //char *tmp_ip_str;
    //char *tmp_port_str;

    tlc_init(); // get ram_size, page_size, dirty_size values of vm 

    memset(paramstr, 0, TLC_STR_LEN);
    memset(valuestr, 0, TLC_STR_LEN);
    memcpy(paramstr, "TLC_MSERV_IP\0", strlen("TLC_MSERV_IP\0"));
    if(dest != NULL){
        memcpy(valuestr, dest, strlen(dest));
    }
    else{
        memcpy(valuestr, "0.0.0.0\0", strlen("0.0.0.0\0"));
    }

    if(strncmp(paramstr,"TLC_MSERV_IP", strlen("TLC_MSERV_IP")) == 0){

        tlc_mserv_ip_str = calloc(TLC_STR_LEN, 1);
        memcpy(tlc_mserv_ip_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, tlc_mserv_ip_str);
    }

    memset(paramstr, 0, TLC_STR_LEN);
    memset(valuestr, 0, TLC_STR_LEN);
    memcpy(paramstr, "TLC_MSERV_BASE_PORT\0", strlen("TLC_MSERV_BASE_PORT\0"));
    memcpy(valuestr, "12300\0", strlen("12300\0"));

    if(strncmp(paramstr,"TLC_MSERV_BASE_PORT", strlen("TLC_MSERV_BASE_PORT")) == 0){
        tlc_mserv_base_port_str = calloc(TLC_STR_LEN, 1);
        memcpy(tlc_mserv_base_port_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, tlc_mserv_base_port_str);
    }

    memset(paramstr, 0, TLC_STR_LEN);
    memset(valuestr, 0, TLC_STR_LEN);
    memcpy(paramstr, "TLC_INTERVAL\0", strlen("TLC_INTERVAL\0"));
    memcpy(valuestr, "1000\0", strlen("1000\0"));

    if(strncmp(paramstr,"TLC_INTERVAL", strlen("TLC_INTERVAL")) == 0){
        tlc_interval_str = calloc(TLC_STR_LEN, 1);
        memcpy(tlc_interval_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, tlc_interval_str);
    }

    memset(paramstr, 0, TLC_STR_LEN);
    memset(valuestr, 0, TLC_STR_LEN);
    memcpy(paramstr, "TLC_SLOWDOWN_INTERVAL\0", strlen("TLC_SLOWDOWN_INTERVAL\0"));
    memcpy(valuestr, "1000\0", strlen("1000\0"));

    if(strncmp(paramstr,"TLC_SLOWDOWN_INTERVAL", strlen("TLC_SLOWDOWN_INTERVAL")) == 0){
        tlc_slowdown_interval_str = calloc(TLC_STR_LEN, 1);
        memcpy(tlc_slowdown_interval_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, tlc_slowdown_interval_str);
    }

    memset(paramstr, 0, TLC_STR_LEN);
    memset(valuestr, 0, TLC_STR_LEN);
    memcpy(paramstr, "TLC_SLOWDOWN_TRIGGER\0", strlen("TLC_SLOWDOWN_TRIGGER\0"));
    memcpy(valuestr, "OFF\0", strlen("OFF\0"));

    if(strncmp(paramstr,"TLC_SLOWDOWN_TRIGGER", strlen("TLC_SLOWDOWN_TRIGGER")) == 0){
        tlc_slowdown_trigger_str = calloc(TLC_STR_LEN, 1);
        memcpy(tlc_slowdown_trigger_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, tlc_slowdown_trigger_str);
    }

    memset(paramstr, 0, TLC_STR_LEN);
    memset(valuestr, 0, TLC_STR_LEN);
    memcpy(paramstr, "TLC_SLOWDOWN_STEPWISE\0", strlen("TLC_SLOWDOWN_STEPWISE\0"));
    memcpy(valuestr, "OFF\0", strlen("OFF\0"));

    if(strncmp(paramstr,"TLC_SLOWDOWN_STEPWISE", strlen("TLC_SLOWDOWN_STEPWISE")) == 0){
        tlc_slowdown_stepwise_str = calloc(TLC_STR_LEN, 1);
        memcpy(tlc_slowdown_stepwise_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, tlc_slowdown_stepwise_str);
    }

    memset(paramstr, 0, TLC_STR_LEN);
    memset(valuestr, 0, TLC_STR_LEN);
    memcpy(paramstr, "TLC_SLOWDOWN_AGGRESSIVENESS\0", strlen("TLC_SLOWDOWN_AGGRESSIVENESS\0"));
    memcpy(valuestr, "0\0", strlen("0\0"));

    if(strncmp(paramstr,"TLC_SLOWDOWN_AGGRESSIVENESS", strlen("TLC_SLOWDOWN_AGGRESSIVENESS")) == 0){
        tlc_slowdown_aggressiveness_str = calloc(TLC_STR_LEN, 1);
        memcpy(tlc_slowdown_aggressiveness_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, tlc_slowdown_aggressiveness_str);
    }

    memset(paramstr, 0, TLC_STR_LEN);
    memset(valuestr, 0, TLC_STR_LEN);
    memcpy(paramstr, "TLC_SLOWDOWN_MARKER\0", strlen("TLC_SLOWDOWN_MARKER\0"));
    memcpy(valuestr, "0\0", strlen("0\0"));

    if(strncmp(paramstr,"TLC_SLOWDOWN_MARKER", strlen("TLC_SLOWDOWN_MARKER")) == 0){
        tlc_slowdown_marker_str = calloc(TLC_STR_LEN, 1);
        memcpy(tlc_slowdown_marker_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, tlc_slowdown_marker_str);
    }

    memset(paramstr, 0, TLC_STR_LEN);
    memset(valuestr, 0, TLC_STR_LEN);
    memcpy(paramstr, "TLC_FREEZE_FLAG\0", strlen("TLC_FREEZE_FLAG\0"));
    memcpy(valuestr, "OFF\0", strlen("OFF\0"));

    if(strncmp(paramstr,"TLC_FREEZE_FLAG", strlen("TLC_FREEZE_FLAG")) == 0){
        tlc_freeze_flag_str = calloc(TLC_STR_LEN, 1);
        memcpy(tlc_freeze_flag_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, tlc_freeze_flag_str);
    }

    memset(paramstr, 0, TLC_STR_LEN);
    memset(valuestr, 0, TLC_STR_LEN);
    memcpy(paramstr, "TLC_FREEZE_INTERVAL\0", strlen("TLC_FREEZE_INTERVAL\0"));
    memcpy(valuestr, "100\0", strlen("100\0"));

    if(strncmp(paramstr,"TLC_FREEZE_INTERVAL", strlen("TLC_FREEZE_INTERVAL")) == 0){
        tlc_freeze_interval_str = calloc(TLC_STR_LEN, 1);
        memcpy(tlc_freeze_interval_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, tlc_freeze_interval_str);
    }

    memset(paramstr, 0, TLC_STR_LEN);
    memset(valuestr, 0, TLC_STR_LEN);
    memcpy(paramstr, "VIC_FLAG\0", strlen("VIC_FLAG\0"));
    memcpy(valuestr, "OFF\0", strlen("OFF\0"));

    if(strncmp(paramstr,"VIC_FLAG", strlen("VIC_FLAG")) == 0){
        vic_flag_str = calloc(TLC_STR_LEN, 1);
        memcpy(vic_flag_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, vic_flag_str);
    }

    memset(paramstr, 0, TLC_STR_LEN);
    memset(valuestr, 0, TLC_STR_LEN);
    memcpy(paramstr, "VIC_IP\0", strlen("VIC_IP\0"));
    memcpy(valuestr, "127.0.0.1\0", strlen("127.0.0.1\0"));

    if(strncmp(paramstr,"VIC_IP", strlen("VIC_IP")) == 0){
        vic_ip_str = calloc(TLC_STR_LEN, 1);
        memcpy(vic_ip_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, vic_ip_str);
    }

    memset(paramstr, 0, TLC_STR_LEN);
    memset(valuestr, 0, TLC_STR_LEN);
    memcpy(paramstr, "VIC_PORT\0", strlen("VIC_PORT\0"));
    memcpy(valuestr, "70001\0", strlen("70001\0"));

    if(strncmp(paramstr,"VIC_PORT", strlen("VIC_PORT")) == 0){
        vic_port_str = calloc(TLC_STR_LEN, 1);
        memcpy(vic_port_str, valuestr, strlen(valuestr));
printf(" tlc_conf: %s = %s \n", paramstr, vic_port_str);
    }

    // Processing given string values 

    if (tlc_mserv_base_port_str!= NULL){
	tlc_mserv_base_port = atoi(tlc_mserv_base_port_str);
    }

    if(tlc_mserv_ip_str != NULL){
	tlc_mserv_ip = tlc_mserv_ip_str;
    }

    if (tlc_interval_str!= NULL){
	tlc_interval = atoi(tlc_interval_str);
    }

    if (tlc_slowdown_interval_str != NULL){
	tlc_slowdown_interval = atoi(tlc_slowdown_interval_str);
    }

    if (tlc_freeze_interval_str != NULL){
	tlc_freeze_interval = atoi(tlc_freeze_interval_str);
    }

    if (vic_flag_str!= NULL){
        if(strncmp(vic_flag_str, "OFF", 3) == 0)
    	  vic_flag = 0;
	else
    	  vic_flag = 1;
    }
    else vic_flag = 0;

    if (vic_port_str!= NULL){
	vic_port = atoi(vic_port_str);
    }

    if(vic_ip_str != NULL)
	vic_ip = vic_ip_str;
//printf("vic_flag = %d VIC PORT = %d VIC IP=%s \n", vic_flag, vic_port, vic_ip);

    if(vic_flag) vic_create_connection(1); // VIC connection

    return 0;
}
