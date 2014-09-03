/*
 * QEMU buffered QEMUFile
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

#include "qemu-common.h"
#include "hw/hw.h"
#include "qemu-timer.h"
#include "qemu-char.h"
#include "buffered_file.h"

#include "tlc-debug.h"

//#define DEBUG_BUFFERED_FILE

typedef struct QEMUFileBuffered
{
    BufferedPutFunc *put_buffer;
    BufferedPutReadyFunc *put_ready;
    BufferedWaitForUnfreezeFunc *wait_for_unfreeze;
    BufferedCloseFunc *close;
    void *opaque;
    QEMUFile *file;
    int freeze_output;
    size_t bytes_xfer;
    size_t xfer_limit;
    uint8_t *buffer;
    size_t buffer_size;
    size_t buffer_capacity;
    QEMUTimer *timer;
} QEMUFileBuffered;

#ifdef DEBUG_BUFFERED_FILE
#define DPRINTF(fmt, ...) \
    do { printf("buffered-file: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

// TLC: declarations
#include <pthread.h>

#define DEFAULT_TLC_INTERVAL  		3000 // ms
#define	DEFAULT_TLC_SLOWDOWN_INTERVAL	1500
#define	DEFAULT_TLC_FREEZE_INTERVAL	100

int tlc_interval =  DEFAULT_TLC_INTERVAL;
int tlc_slowdown_interval =  DEFAULT_TLC_SLOWDOWN_INTERVAL;
int tlc_freeze_interval =  DEFAULT_TLC_FREEZE_INTERVAL;

extern int 		mthread;

extern int64_t 		mig_period_start, mig_period_end, 
			mig_duration, acc_vm_overhead, max_mig_duration;
extern int 		duration_id;
extern int64_t 		stage_start, stage_finish;

extern int64_t 		epoch_start, epoch_end, epoch_duration;
extern uint64_t 	pages_copied_last_epoch; 
extern uint64_t 	pages_gen_last_epoch;

extern unsigned int physical_ram_size;

//extern int final_migration_cleanup(void *opaque);

// TLC: worker thread
extern pthread_mutex_t mutex_save_page; 
extern pthread_mutex_t mutex_theone;
extern int stage2_done;
extern int nodirty; 
extern pthread_cond_t  have_dirty;
extern uint64_t tlc_vm_old_pages, tlc_vm_new_pages;

static void buffered_append(QEMUFileBuffered *s,
                            const uint8_t *buf, size_t size)
{
    if (size > (s->buffer_capacity - s->buffer_size)) {
        void *tmp;

        DPRINTF("increasing buffer capacity from %zu by %zu\n",
                s->buffer_capacity, size + 1024);

        s->buffer_capacity += size + 1024;

        tmp = g_realloc(s->buffer, s->buffer_capacity);
        if (tmp == NULL) {
            fprintf(stderr, "qemu file buffer expansion failed\n");
            exit(1);
        }

        s->buffer = tmp;
    }

    memcpy(s->buffer + s->buffer_size, buf, size);
    s->buffer_size += size;
}

static void buffered_flush(QEMUFileBuffered *s)
{
    size_t offset = 0;
    int error;

    error = qemu_file_get_error(s->file);
    if (error != 0) {
        DPRINTF("flush when error, bailing: %s\n", strerror(-error));
        return;
    }

    DPRINTF("flushing %zu byte(s) of data\n", s->buffer_size);

    while (offset < s->buffer_size) {
        ssize_t ret;

        ret = s->put_buffer(s->opaque, s->buffer + offset,
                            s->buffer_size - offset);
        if (ret == -EAGAIN) {
            DPRINTF("backend not ready, freezing\n");
DVERYDETAIL{
printf("bflush : backend not ready, freezing\n");
fflush(stdout);}
            s->freeze_output = 1;
            break;
        }

        if (ret <= 0) {
            DPRINTF("error flushing data, %zd\n", ret);
DVERYDETAIL{
printf("bflush :error flushing data, %zd\n", ret);
fflush(stdout);}

            qemu_file_set_error(s->file, ret);
            break;
        } else {
            DPRINTF("flushed %zd byte(s)\n", ret);
            offset += ret;
        }
    }
    DPRINTF("flushed %zu of %zu byte(s)\n", offset, s->buffer_size);
    memmove(s->buffer, s->buffer + offset, s->buffer_size - offset);
    s->buffer_size -= offset;

}

static int buffered_put_buffer(void *opaque, const uint8_t *buf, int64_t pos, int size)
{
    QEMUFileBuffered *s = opaque;
    int offset = 0, error;
    ssize_t ret;

    error = qemu_file_get_error(s->file);
    if (error) {
        DPRINTF("flush when error, bailing: %s\n", strerror(-error));
        return error;
    }

    DPRINTF("unfreezing output\n");
    s->freeze_output = 0;

    buffered_flush(s);

    while (!s->freeze_output && offset < size) {
        if (s->bytes_xfer > s->xfer_limit) {
            DPRINTF("transfer limit exceeded when putting\n");
            break;
        }

        ret = s->put_buffer(s->opaque, buf + offset, size - offset);
        if (ret == -EAGAIN) {
            s->freeze_output = 1;
            break;
        }

        if (ret <= 0) {
            qemu_file_set_error(s->file, ret);
            offset = -EINVAL;
            break;
        }

        offset += ret;
        s->bytes_xfer += ret;
    }

    if (offset >= 0) {
        buffered_append(s, buf + offset, size - offset);
        offset = size;
    }

    if(unlikely(!mthread)){
    // Don't do this when doing tlc. I want to be in control.
    // Or, we may add more codes to handle tlc ops here later.
    if (pos == 0 && size == 0) {
        DPRINTF("file is ready\n");
        if (s->bytes_xfer <= s->xfer_limit) {
            DPRINTF("notifying client\n");
            s->put_ready(s->opaque);
        }
    }
    
    }
    return offset;
}

static int buffered_close(void *opaque)
{
    QEMUFileBuffered *s = opaque;
    int ret;

    DPRINTF("closing\n");

    while (!qemu_file_get_error(s->file) && s->buffer_size) {
        buffered_flush(s);
        if (s->freeze_output)
            s->wait_for_unfreeze(s->opaque);
    }

    ret = s->close(s->opaque);

    qemu_del_timer(s->timer);
    qemu_free_timer(s->timer);
    g_free(s->buffer);
    g_free(s);

    return ret;
}

/*
 * The meaning of the return values is:
 *   0: We can continue sending
 *   1: Time to stop
 *   negative: There has been an error
 */
static int buffered_rate_limit(void *opaque)
{
    QEMUFileBuffered *s = opaque;
    int ret;
DVERYDETAIL{printf("buff rate limit: in\n");
fflush(stdout);}
    ret = qemu_file_get_error(s->file);
    if (ret) {
        return ret;
    }
    if (s->freeze_output)
        return 1;

    if (s->bytes_xfer > s->xfer_limit)
        return 1;

    return 0;
}

static int64_t buffered_set_rate_limit(void *opaque, int64_t new_rate)
{
    QEMUFileBuffered *s = opaque;
DVERYDETAIL{printf("buff set rate limit: in\n");
fflush(stdout);}
    if (qemu_file_get_error(s->file)) {
        goto out;
    }
    if (new_rate > SIZE_MAX) {
        new_rate = SIZE_MAX;
    }

    s->xfer_limit = new_rate / 10;
    
out:
    return s->xfer_limit;
}

static int64_t buffered_get_rate_limit(void *opaque)
{
    QEMUFileBuffered *s = opaque;
DVERYDETAIL{printf("buff get rate limit: in\n");
fflush(stdout);}
    return s->xfer_limit;
}

static void buffered_rate_tick(void *opaque)
{
    QEMUFileBuffered *s = opaque;
DVERYDETAIL{printf("in brt\n");
fflush(stdout);}
    if (qemu_file_get_error(s->file)) {
        buffered_close(s);
        return;
    }

    qemu_mod_timer(s->timer, qemu_get_clock_ms(rt_clock) + 100);

    if (s->freeze_output)
        return;

    s->bytes_xfer = 0;

    buffered_flush(s);

    /* Add some checks around this */
DVERYDETAIL{printf("in brt calling mfpr\n");
fflush(stdout);}
    s->put_ready(s->opaque);
}
// Functions below are TLC's mechanisms to periodically 
// save state (running on io-thread)
#include "qapi-types.h"
extern void vm_stop(RunState state);
extern void vm_start(void);
extern int tlc_handle_dirty_ram(void);
//extern void mc_close(void);
//extern void mc_close2(void);

// TLC slowdown
#include <sched.h>
#include <math.h>
#include "tlc.h" // profiling plus VIC stuffs

extern int smp_cpus; 
extern int cpu_break_switch;
//TLC profiling
extern tlc_profile_type *tlc_profile;

extern void resume_all_vcpus(void);
extern void pause_all_vcpus(void);

cpu_set_t 	adjusted_cpu_set; 
int 		slowdown_aggressiveness = 0; 
double		slowdown_progress_unit;
int		stepwise_reduction_counter = 0;

extern 		int 	trigger_slowdown;
extern 		double 	slowdown_marker;

typedef uint64_t ram_addr_t;
extern 		ram_addr_t	vm_last_ram_offset;

extern		uint64_t  stage2_last_addr;
extern 		pthread_rwlock_t dirty_page_rwlock;

// TLC average page sent and gen speed calculation
extern double epoch_pages_gen_speed; //  = pages gen/epoch duration
extern double aggregate_pages_gen;
extern double avg_pages_gen_per_epoch; // = agg pages gen/num of epoch

extern double epoch_pages_sent_speed; // = pages sent/epoch duration
extern double aggregate_pages_sent;
extern double avg_pages_sent_per_epoch; // = agg pages sent/num of epoch

extern double aggregate_epoch_duration; 

extern double avg_pages_gen_speed;  // = agg pages gen/agg epoch duration
extern double avg_pages_sent_speed;  // = agg pages sent/agg epoch duration

extern uint64_t tlc_ram_page_remaining(void);

// VIC stuffs
extern int vic_flag;
extern int vic_report(char *message);

static uint64_t tlc_io_profile_report(void){
    int i;
    uint64_t all_io_cnt = 0;
    char progress_string[VIC_STR_LEN];

    for(i = 0; i < smp_cpus; i++){
	all_io_cnt += tlc_profile[i].total_io_cnt;
	tlc_profile[i].total_io_cnt = 0; // reset all io counts
    }
    //printf(" all_io_cnt = %" PRId64 "\n", all_io_cnt);

    if(vic_flag){
        snprintf(progress_string, VIC_STR_LEN,"ct 100 %" PRId64 " ", all_io_cnt);
        vic_report(progress_string);
    }
    return all_io_cnt; 
}

extern void tlc_display_ram_list(void); // TLC ram

int slowdown_cpu(int reduced_num_cpus);

int slowdown_cpu(int reduced_num_cpus){

        int x;
        char progress_string[VIC_STR_LEN];

	if((reduced_num_cpus > smp_cpus) || (smp_cpus < 2)){ 
	    printf("slowdown_cpu: No slowdown (reduced num cpus = %d smp = %d)\n", 
		reduced_num_cpus, smp_cpus);
	    return 0; 
	}

	// TLC ram
	//tlc_display_ram_list();
		    
printf("cpus %d\n", reduced_num_cpus);
        if(vic_flag){
            snprintf(progress_string, VIC_STR_LEN,"cpus %d", reduced_num_cpus);
            vic_report(progress_string);
        }

	// set new affinity template here
        CPU_ZERO(&adjusted_cpu_set);

        for(x = 0; x < reduced_num_cpus; x++){
            CPU_SET(x, &adjusted_cpu_set);
	}

        // switch used by io_thr and vcpus 
        cpu_break_switch = 1;
        pause_all_vcpus(); // syn io_thr and vcpus and apply the adjusted affinity set 
		
        cpu_break_switch = 0;
        resume_all_vcpus();

	return 1;
}

extern void print_current_vcpu_info(int cpu_index);
extern void save_vcpu_affinity(void);
extern void restore_vcpu_affinity(void);

extern int state_transfer_type;
extern int state_transfer_mode;
extern int stepwise_slowdown;

extern int64_t s2_end_time; 
extern int64_t accum_page_saver_wait_time; 

#define MIN_EXE_RATIO ((double) 0.1)
#define MIN_GEN_PAGES (500)

extern int freeze_flag; 

int64_t std_tlc_interval, acc_tlc_interval, min_tlc_std_exetime;
double freeze_std_ratio = 1;
int first_freeze_interval = 0;
int64_t Bi = 0; // last exe epoch time
int64_t std_pause_time = 0;
int64_t norm_pause_time = 0;
int periodic_freeze_flag = 0;

// Types of algorithms for determining pausing time

#define SHARP_REDUCE_DOUBLE_INCREASE_ALGOR 1
#define SHARP_REDUCE_AVERAGE_INCREASE_ALGOR 2
#define AVERAGE_REDUCE_AVERAGE_INCREASE_ALGOR 3
#define FIXED_FREEZE_EXETIME_ALGOR 4
#define FREEZE_FINAL_VCPUS_ALGOR 5

//int freezing_algor_type = SHARP_REDUCE_DOUBLE_INCREASE_ALGOR;
int freezing_algor_type = AVERAGE_REDUCE_AVERAGE_INCREASE_ALGOR;

int min_exe_burst; // Smallest exe burst will produce tlc_min_std_exetime.

int64_t cal_fixed_freeze_per_std_interval(int64_t Si, int64_t Gi);
int64_t cal_fixed_freeze_per_std_interval(int64_t Si, int64_t Gi){

    return(std_tlc_interval - min_tlc_std_exetime);
}

int64_t cal_freeze_per_std_interval(int64_t Si, int64_t Gi);
int64_t cal_freeze_per_std_interval(int64_t Si, int64_t Gi){
    int64_t Biplus1 = 0;

    if(Gi <= MIN_GEN_PAGES){
        Biplus1 = std_tlc_interval;
printf("fr_cal G<1000 : std = %" PRId64 " \n", std_tlc_interval);
fflush(stdout);
    }
    //else if((Gi >= Si - MIN_GEN_PAGES) && (Gi <= Si + MIN_GEN_PAGES)){
    //    Biplus1 = Bi;
    //}
    else if(Gi > Si +  MIN_GEN_PAGES){

printf("fr_cal G>S 0: exp1 %lf 2 %" PRId64 " 3 %" PRId64 " \n", 
	(double) (((double)Bi) * (((double)Si)/((double)Gi))),
	(int64_t) (((double)Bi) * (((double)Si)/((double)Gi))),
	(Bi * (Si/Gi))
	);
fflush(stdout);
        if(((double) (((double)Bi) * (((double)Si)/((double)Gi)))) > min_tlc_std_exetime){

          Biplus1 = (int64_t) (((double)Bi) * (((double)Si)/((double)Gi))); 
printf("fr_cal G>S 1: %lf \n", (double) (((double)Bi) * (((double)Si)/((double)Gi))));
fflush(stdout);
        }
        else{
          Biplus1 = min_tlc_std_exetime; 
printf("fr_cal G>S 2: min = %" PRId64 "\n", min_tlc_std_exetime);
fflush(stdout);
        }

    }
/*
    else if(Gi < Si - MIN_GEN_PAGES){

        if(std_tlc_interval < 2 * Bi){
          Biplus1 = std_tlc_interval; 
printf("fr_cal G<S 1: std = %" PRId64 " \n", std_tlc_interval);
fflush(stdout);
        }
        else{
          Biplus1 = 2 * Bi; 
printf("fr_cal G<S 2\n");
fflush(stdout);
        }
    }
*/
    else{ 
        Biplus1 = std_tlc_interval;
printf("fr_cal G!>S don't pause std = %" PRId64 " \n", std_tlc_interval);
fflush(stdout);
/*
        Biplus1 = Bi;
printf("fr_cal 3: else keep same Bi\n");
fflush(stdout);
*/
    }

printf("fr_cal Si %" PRId64 " Gi %" PRId64 " Bi %" PRId64 " Bi+1 %" PRId64 " pause_time %" PRId64 " \n", Si, Gi, Bi, Biplus1, (std_tlc_interval - Biplus1));
fflush(stdout);

    Bi = Biplus1;

    return(std_tlc_interval - Biplus1);
}

double acc_Gi = 0.0;
int num_fr_du; 

int64_t cal_avg_freeze_per_std_interval(int64_t Si, int64_t Gi);

int64_t cal_avg_freeze_per_std_interval(int64_t Si, int64_t Gi){

    int64_t Biplus1 = 0;
    //double avg_Gi = 0.0; 

    //acc_Gi += Gi;
    //avg_Gi = (double)(acc_Gi/(double) num_fr_du);

    if(Gi <= MIN_GEN_PAGES){
        Biplus1 = std_tlc_interval;
printf("fr_cal2 G<1000 : std = %" PRId64 " \n", std_tlc_interval);
fflush(stdout);
    }
    else if(Gi > Si +  MIN_GEN_PAGES){
	double tmp_B = ((double) (((double)Bi) * (((double)Si)/((double)Gi))));
printf("fr_cal2 G>S 0: Bi*Si/Gi = %lf 2 %" PRId64 " \n", tmp_B, (int64_t) tmp_B);
fflush(stdout);
        if(tmp_B > min_tlc_std_exetime){

          Biplus1 = (int64_t) (tmp_B); 
printf("fr_cal2 G>S 1: %lf \n", tmp_B);
fflush(stdout);
        }
        else{
          Biplus1 = min_tlc_std_exetime; 
printf("fr_cal2 G>S 2: min = %" PRId64 "\n", min_tlc_std_exetime);
fflush(stdout);
        }

    }
    else if(Gi < Si - MIN_GEN_PAGES){
	//double tmp_B = ((double) (((double)Bi) * (((double)Si)/((double)avg_Gi))));
	double tmp_B = ((double) (((double)Bi) * (((double)Si)/((double)Gi))));
printf("fr_cal2 G<S 0: Bi*Si/Gi = %lf \n", tmp_B);
fflush(stdout);

        if(tmp_B < std_tlc_interval){
	    if(tmp_B > min_tlc_std_exetime){
          	Biplus1 = (int64_t) (tmp_B); 
printf("fr_cal2 G<S 1: Bi+1 %lf \n", (double) (tmp_B));
fflush(stdout);
	    }
	    else{
                Biplus1 = min_tlc_std_exetime; 
printf("fr_cal2 G<S 2: min = %" PRId64 "\n", min_tlc_std_exetime);
fflush(stdout);
	    }
        }
        else{
          Biplus1 = std_tlc_interval; 
printf("fr_cal2 G<S 3: std = %" PRId64 " \n", std_tlc_interval);
fflush(stdout);
        }

    }
    else{
        Biplus1 = Bi;
printf("fr_cal2 3: else keep same Bi\n");
fflush(stdout);
    }

//printf("fr_cal2 Si %" PRId64 " Gi %" PRId64 " Bi %" PRId64 " Bi+1 %" PRId64 " pause_time %" PRId64 " (aG = %lf) \n", Si, Gi, Bi, Biplus1, (std_tlc_interval - Biplus1), avg_Gi);
printf("fr_cal2 Si %" PRId64 " Gi %" PRId64 " Bi %" PRId64 " Bi+1 %" PRId64 " pause_time %" PRId64 " \n", Si, Gi, Bi, Biplus1, (std_tlc_interval - Biplus1));
fflush(stdout);

    Bi = Biplus1;

    return(std_tlc_interval - Biplus1);
}

int64_t cal_avgall_freeze_per_std_interval(int64_t Si, int64_t Gi);
int64_t cal_avgall_freeze_per_std_interval(int64_t Si, int64_t Gi){

    int64_t Biplus1 = 0;
    double avg_Gi = 0.0; 

    acc_Gi += Gi;
    avg_Gi = (double)(acc_Gi/(double) num_fr_du);

    if(Gi <= MIN_GEN_PAGES){
        Biplus1 = std_tlc_interval;
printf("fr_cal2 G<1000 : std = %" PRId64 " \n", std_tlc_interval);
fflush(stdout);
    }
    else if(Gi > Si +  MIN_GEN_PAGES){
//printf("fr_cal2 G>S 0: exp1 %lf 2 %" PRId64 " 3 %" PRId64 " \n", 
//	(double) (((double)Bi) * (((double)Si)/((double)Gi))),
//	(int64_t) (((double)Bi) * (((double)Si)/((double)Gi))),
//	(Bi * (Si/Gi))
//	);
//fflush(stdout);
        if(((double) (((double)Bi) * (((double)Si)/((double)avg_Gi)))) > min_tlc_std_exetime){

          Biplus1 = (int64_t) (((double)Bi) * (((double)Si)/((double)avg_Gi))); 
printf("fr_cal2 G>S 1: %lf \n", (double) (((double)Bi) * (((double)Si)/((double)avg_Gi))));
fflush(stdout);
        }
        else{
          Biplus1 = min_tlc_std_exetime; 
printf("fr_cal2 G>S 2: min = %" PRId64 "\n", min_tlc_std_exetime);
fflush(stdout);
        }

    }
    else if(Gi <= MIN_GEN_PAGES){
        Biplus1 = std_tlc_interval;
printf("fr_cal2 G<1000 : std = %" PRId64 " \n", std_tlc_interval);
fflush(stdout);
    }
    else if(Gi < Si - MIN_GEN_PAGES){
	double tmp_B = ((double) (((double)Bi) * (((double)Si)/((double)avg_Gi))));
printf("fr_cal2 G<S 0: exp1 %lf \n", tmp_B);
fflush(stdout);

        if(tmp_B < std_tlc_interval){
	    if(tmp_B > min_tlc_std_exetime){
          	Biplus1 = (int64_t) (((double)Bi) * (((double)Si)/((double)avg_Gi))); 
printf("fr_cal2 G<S 1: Bi+1 %lf \n", (double) (((double)Bi) * (((double)Si)/((double)avg_Gi))));
fflush(stdout);
	    }
	    else{
                Biplus1 = min_tlc_std_exetime; 
printf("fr_cal2 G<S 2: min = %" PRId64 "\n", min_tlc_std_exetime);
fflush(stdout);
	    }
        }
        else{
          Biplus1 = std_tlc_interval; 
printf("fr_cal2 G<S 3: std = %" PRId64 " \n", std_tlc_interval);
fflush(stdout);
        }

    }
    else{
        Biplus1 = Bi;
printf("fr_cal2 3: else keep same Bi\n");
fflush(stdout);
    }

printf("fr_cal2 Si %" PRId64 " Gi %" PRId64 " Bi %" PRId64 " Bi+1 %" PRId64 " pause_time %" PRId64 " (aG = %lf) \n", Si, Gi, Bi, Biplus1, (std_tlc_interval - Biplus1), avg_Gi);
fflush(stdout);

    Bi = Biplus1;

    return(std_tlc_interval - Biplus1);
}

void tlc_ms_sleep(int64_t pause_time);
void tlc_ms_sleep(int64_t pause_time){
    int64_t start_sleep, stop_sleep, sleep_elasp; 

    start_sleep = qemu_get_clock_ms(rt_clock);
    while(1){
      stop_sleep = qemu_get_clock_ms(rt_clock);
      sleep_elasp = (stop_sleep - start_sleep); 
      if(sleep_elasp > pause_time){
        //printf("norm pause (param)= %" PRId64 " paused time = %" PRId64 " ms \n", pause_time, sleep_elasp);
        //fflush(stdout);
        return;
      }
    }
}

extern void clear_sleep_penv(void);
extern void add_sleep_to_cpu(int sleep_time, int final_cpu_num);

int orig_vcpus_saved = 0;

static void buffered_rate_tick_m(void *opaque)
{
    QEMUFileBuffered *s = opaque;
    //int tmp_stage2_done = 0;

    uint64_t all_io_cnt = 0;
    double stage2_progress;
    char progress_string[VIC_STR_LEN];
    
    // TLC timing
    mig_period_start = qemu_get_clock_ms(rt_clock);
    duration_id++; 
    
    epoch_end = mig_period_start;
    epoch_duration = epoch_end - epoch_start;

    if(periodic_freeze_flag == 1){
        acc_tlc_interval += tlc_freeze_interval; 
    }

/*
    DVIC{
       if(vic_flag){
           sprintf(progress_string, "s221 %d %" PRId64 " %" PRId64 " \n", duration_id, pages_copied_last_epoch, epoch_duration);
	   vic_report(progress_string);
       }

       //printf("s221 %" PRId64 " %" PRId64 " \n", pages_copied_last_epoch, epoch_duration);
    }
*/
    pages_gen_last_epoch = 0; 
    
    if(unlikely(!mthread)){
    	printf("Fatal error: brtm called with mthred reset!\n");
	fflush(stdout);
    	return ;
    }
    s->bytes_xfer = 0; 
        
    pthread_mutex_lock(&mutex_save_page);    
    //tmp_stage2_done = stage2_done;

    if (stage2_done){    
	
            int64_t s3_start_time, s2s3gap_duration; 

	    // PRE
       	    printf("s221 %" PRId64 " %" PRId64 " \n", pages_copied_last_epoch, epoch_duration);

            stage2_progress = 1.0; // done 
            s3_start_time = mig_period_start; 
            s2s3gap_duration = s3_start_time - s2_end_time;
            printf("s2s3gap %" PRId64 " ps_wait %" PRId64 "\n", s2s3gap_duration, accum_page_saver_wait_time);
            fflush(stdout);
	
	    // VIC stuff
	    if(vic_flag){
	        //strncpy(progress_string, "ct 100", VIC_STR_LEN);
	        vic_report((char *)"ct 100");
	    }

    	    buffered_flush(s);	
	    s->put_ready(s->opaque); // call ram save live

            if(orig_vcpus_saved == 1){
	      if(
		(state_transfer_type == TLC_EXEC)||
		(state_transfer_type == TLM_EXEC)
	        ){
            	  restore_vcpu_affinity();
		  orig_vcpus_saved = 0;
		  print_current_vcpu_info(0);
	      }
            }
	//DREG{printf("%d:       PagesGen_when_start_S3: %" PRId64 " \n", pages_gen_last_epoch);}

	    //mc_close2();

    	    tlc_vm_old_pages = 0;
    	    tlc_vm_new_pages = 0;   
	    pages_copied_last_epoch = 0; // reset number of copied pages

            pthread_mutex_unlock(&mutex_theone); //  unlock PS thread

	    pthread_mutex_unlock(&mutex_save_page);

	    // POST
            //if(tmp_stage2_done){
	    all_io_cnt = tlc_io_profile_report();
            //}

    	    // TLC VM timing
            mig_period_end = qemu_get_clock_ms(rt_clock);
    	    epoch_start = mig_period_end;
    
    	    mig_duration = mig_period_end - mig_period_start;
    	    acc_vm_overhead += mig_duration;
    	    if(max_mig_duration < mig_duration) max_mig_duration = mig_duration;

	    printf("s222 %" PRId64 " %" PRId64 " %" PRId64 " %.2lf\n", pages_gen_last_epoch, mig_duration, all_io_cnt, stage2_progress);
            fflush(stdout);
    } 
    else{

      if(state_transfer_mode == TLM_ONEPASS_MODE){
	    pthread_mutex_unlock(&mutex_save_page);
	    qemu_mod_timer(s->timer, qemu_get_clock_ms(rt_clock) + tlc_interval);

	    pages_copied_last_epoch = 0; // reset copied page counter variables
      }
      else{
	    uint64_t local_stage2_last_addr; 
	    
            if(periodic_freeze_flag == 1){

                if((first_freeze_interval == 1) || 
                   (acc_tlc_interval >= std_tlc_interval)){

                    acc_tlc_interval = 0;

		    // PRE
       		    printf("s221 %" PRId64 " %" PRId64 " \n", pages_copied_last_epoch, epoch_duration);

	            pthread_rwlock_wrlock(&dirty_page_rwlock); 
	            tlc_handle_dirty_ram();
	            local_stage2_last_addr = stage2_last_addr;
	            pthread_rwlock_unlock(&dirty_page_rwlock); 

                    if(first_freeze_interval == 1){ 
			first_freeze_interval = 0;
                        num_fr_du = 1; 
			acc_Gi = 0;
			clear_sleep_penv();
                        Bi = std_tlc_interval; 

                        switch(freezing_algor_type){

			case FIXED_FREEZE_EXETIME_ALGOR:
                        	std_pause_time = cal_fixed_freeze_per_std_interval(pages_copied_last_epoch, pages_gen_last_epoch);
				break;
			case SHARP_REDUCE_AVERAGE_INCREASE_ALGOR:
			case FREEZE_FINAL_VCPUS_ALGOR:
                        	std_pause_time = cal_avg_freeze_per_std_interval(pages_copied_last_epoch, pages_gen_last_epoch);
				break;
			case SHARP_REDUCE_DOUBLE_INCREASE_ALGOR: 
                        	std_pause_time = cal_freeze_per_std_interval(pages_copied_last_epoch, pages_gen_last_epoch);
				break;
			case AVERAGE_REDUCE_AVERAGE_INCREASE_ALGOR: 
			default: 
                        	std_pause_time = cal_avgall_freeze_per_std_interval(pages_copied_last_epoch, pages_gen_last_epoch);
				break;
			}

                        norm_pause_time = (int64_t) (((double)std_pause_time) * freeze_std_ratio);
                    }
                    else{
                        // calculate pause time (at least 10 ms) for 
                        // use in the next tlc_freeze_interval
                        num_fr_du++; 

                        switch(freezing_algor_type){

			case FIXED_FREEZE_EXETIME_ALGOR:
                        	std_pause_time = cal_fixed_freeze_per_std_interval(pages_copied_last_epoch, pages_gen_last_epoch);
				break;
			case SHARP_REDUCE_AVERAGE_INCREASE_ALGOR:
			case FREEZE_FINAL_VCPUS_ALGOR:
                        	std_pause_time = cal_avg_freeze_per_std_interval(pages_copied_last_epoch, pages_gen_last_epoch);
				break;
			case SHARP_REDUCE_DOUBLE_INCREASE_ALGOR: 
                        	std_pause_time = cal_freeze_per_std_interval(pages_copied_last_epoch, pages_gen_last_epoch);
				break;
			case AVERAGE_REDUCE_AVERAGE_INCREASE_ALGOR: 
			default: 
                        	std_pause_time = cal_avgall_freeze_per_std_interval(pages_copied_last_epoch, pages_gen_last_epoch);
				break;
			}

                        norm_pause_time = (int64_t) (((double)std_pause_time) * freeze_std_ratio);
                    }
        	    printf("norm pause time = %" PRId64 " ms \n", norm_pause_time);
        	    fflush(stdout);

                    tlc_interval = tlc_freeze_interval;  // TLC FRZ

	            nodirty = 0; 
	            pthread_cond_signal(&have_dirty);
	            pthread_mutex_unlock(&mutex_save_page);

	            stage2_progress = ((double)local_stage2_last_addr / (double)vm_last_ram_offset);

	            qemu_mod_timer(s->timer, qemu_get_clock_ms(rt_clock) + tlc_interval);

	            pages_copied_last_epoch = 0; 

		    // POST
    	            // TLC VM timing
            	    mig_period_end = qemu_get_clock_ms(rt_clock);
    	    	    epoch_start = mig_period_end;
    
    	    	    mig_duration = mig_period_end - mig_period_start;
    	    	    acc_vm_overhead += mig_duration;
    	    	    if(max_mig_duration < mig_duration) max_mig_duration = mig_duration;

		    printf("s222 %" PRId64 " %" PRId64 " %" PRId64 " %.2lf\n", pages_gen_last_epoch, mig_duration, all_io_cnt, stage2_progress);
        	    fflush(stdout);

                }
                else{

                    if(nodirty){
                      // make next tlc interval short and then force du
                      Bi = std_tlc_interval; // set execution time to full epoch 
                      tlc_interval = min_exe_burst;  
                      acc_tlc_interval = std_tlc_interval;

	              pthread_mutex_unlock(&mutex_save_page);
                      // in the future if G is small enough... can enter stage 3 here
                    }
                    else{
	              pthread_mutex_unlock(&mutex_save_page);


                      //vm_stop(RUN_STATE_PAUSED);
                      //freeze_du_start = qemu_get_clock_ms(rt_clock);
		      if (freezing_algor_type == FREEZE_FINAL_VCPUS_ALGOR){

                          tlc_interval = tlc_freeze_interval - norm_pause_time;  

			  add_sleep_to_cpu(norm_pause_time, slowdown_aggressiveness);
		      }
  		      else{
                          tlc_interval = tlc_freeze_interval - norm_pause_time;  
		          pause_all_vcpus();
                          tlc_ms_sleep(norm_pause_time);
		          resume_all_vcpus();
		      }
                      //vm_start();

                      //freeze_du_end = qemu_get_clock_ms(rt_clock);
                      //freeze_du_elasp = freeze_du_end = freeze_du_start; 

                    // print out these time to check if freeze_du_elasp equal pause time 
                    }

	            stage2_progress = 0;

	            qemu_mod_timer(s->timer, qemu_get_clock_ms(rt_clock) + tlc_interval);

                }
            }
            else{ // ! freeze_flag

	    // PRE
       	    printf("s221 %" PRId64 " %" PRId64 " \n", pages_copied_last_epoch, epoch_duration);

	    pthread_rwlock_wrlock(&dirty_page_rwlock); 
	    tlc_handle_dirty_ram();
	    
	    local_stage2_last_addr = stage2_last_addr;
	    pthread_rwlock_unlock(&dirty_page_rwlock); 

	    if((trigger_slowdown == 1)&&(freeze_flag == 1)){
              // need to do this before unlock mutex
              // here! calculate the stop time and stop vm for 
              // a certain amount of time (ms) 
              // calculate the rest of time to execute (at least 10 ms)
              norm_pause_time = std_pause_time = 0;
              acc_tlc_interval = 0;
              periodic_freeze_flag = 1;

	      tlc_interval = tlc_slowdown_interval; 
              std_tlc_interval = tlc_interval;
              freeze_std_ratio = ((double)tlc_freeze_interval)/((double)std_tlc_interval);

	      if(freeze_std_ratio > 0){
		  // In the new protocol, user must define the min_exe_burst parameter value.
                  min_tlc_std_exetime = (double)(min_exe_burst)/freeze_std_ratio;
              }
	      else{
                  min_tlc_std_exetime = MIN_EXE_RATIO * ((double)(std_tlc_interval));
		  printf("Error freeze_std_ratio <= 0, min = %" PRId64 " \n", min_tlc_std_exetime);
		  fflush(stdout);
	      }

              first_freeze_interval = 1;
// TLC
printf("Initialize Freezing total min= %" PRId64 " \n", min_tlc_std_exetime);
fflush(stdout);
            }
            
	    nodirty = 0;
	    pthread_cond_signal(&have_dirty);
	    pthread_mutex_unlock(&mutex_save_page);

	    stage2_progress = ((double)local_stage2_last_addr / (double)vm_last_ram_offset);

	    // TLC slowdown
	    if(trigger_slowdown == 1){

	        if (stage2_progress >= slowdown_marker){  
		    // report progress and io cnt via VIC
    		    int i;
		    for(i = 0; i < smp_cpus; i++){
	 		all_io_cnt += tlc_profile[i].total_io_cnt;
	 		tlc_profile[i].total_io_cnt = 0; // reset all io counts
		    }

	            if(vic_flag){
	                snprintf(progress_string, VIC_STR_LEN,"ct %d %" PRId64 " ", 
				(int)(stage2_progress * 100.0), all_io_cnt);
	                vic_report(progress_string);
	            }

                    if(slowdown_aggressiveness > smp_cpus){
                        trigger_slowdown = 0; // disable slowdown trigger
                        printf("No slowdown! agressiveness (%d) exceed smp_cpus (%d) \n", slowdown_aggressiveness, smp_cpus);
                        fflush(stdout);
                    }
                    else{

		    if(!stepwise_slowdown){
	    	    	trigger_slowdown = 0; // disable slowdown trigger

	    		if(
				(state_transfer_type == TLC_EXEC)||
				(state_transfer_type == TLM_EXEC)
	      		  ){
				save_vcpu_affinity();
				orig_vcpus_saved = 1;
	    		}

			slowdown_cpu(slowdown_aggressiveness);

		    }
		    else{

			if(stepwise_reduction_counter == 0){
			    // calculate progress per reduction step unit
			    slowdown_progress_unit = 
				(1.0 - slowdown_marker)/
				((double)(smp_cpus - slowdown_aggressiveness));
			    // change epoch interval (if different) 
		    	    tlc_interval = tlc_slowdown_interval; 

	    		    if(
				(state_transfer_type == TLC_EXEC)||
				(state_transfer_type == TLM_EXEC)
	      		      ){
				save_vcpu_affinity();
				orig_vcpus_saved = 1;
	    		    }

			}

			stepwise_reduction_counter++;
			// call slowdown
			if(!slowdown_cpu(smp_cpus - stepwise_reduction_counter)){
	    	    	    trigger_slowdown = 0; 
			}
printf("slow redcount= %d smp = %d aggress = %d \n", 
	stepwise_reduction_counter, smp_cpus, slowdown_aggressiveness);
			if(stepwise_reduction_counter < (smp_cpus - slowdown_aggressiveness)){	
			    // calculate next slowdown_marker
			    slowdown_marker += slowdown_progress_unit;
printf("slowUnit %.4lf at %.4lf next %.4lf\n", 
	slowdown_progress_unit, stage2_progress, slowdown_marker);
	            	    if(vic_flag){
	                	snprintf(progress_string, VIC_STR_LEN,
					"slowUnit %.4lf at %.4lf next %.4lf", 
					slowdown_progress_unit, stage2_progress, slowdown_marker);
	                	vic_report(progress_string);
	            	    }
			}
			else{
	    	    	    trigger_slowdown = 0; 
			}

		    }

		    }
	        }
		else{
	            // VIC stuffs
	            if(vic_flag){
	                snprintf(progress_string, VIC_STR_LEN,"ct %d", (int)(stage2_progress * 100.0));
	                vic_report(progress_string);
	            }

		}

	    }
	    else{
	        // VIC stuffs
	        if(vic_flag){
	            snprintf(progress_string, VIC_STR_LEN,"ct %d", (int)(stage2_progress * 100.0));
	            vic_report(progress_string);
	        }

	    }

	    qemu_mod_timer(s->timer, qemu_get_clock_ms(rt_clock) + tlc_interval);

	    pages_copied_last_epoch = 0; // reset copied page counter variables

	    // POST
    	    // TLC VM timing
            mig_period_end = qemu_get_clock_ms(rt_clock);
    	    epoch_start = mig_period_end;
    
    	    mig_duration = mig_period_end - mig_period_start;
    	    acc_vm_overhead += mig_duration;
    	    if(max_mig_duration < mig_duration) max_mig_duration = mig_duration;

	    printf("s222 %" PRId64 " %" PRId64 " %" PRId64 " %.2lf\n", pages_gen_last_epoch, mig_duration, all_io_cnt, stage2_progress);
            fflush(stdout);

            } // !freeze+flag

      } // state_transfer_mode
    }
    // TLC VM timing
    //mig_period_end = qemu_get_clock_ms(rt_clock);
    //epoch_start = mig_period_end;
    
    //mig_duration = mig_period_end - mig_period_start;
    //acc_vm_overhead += mig_duration;
    //if(max_mig_duration < mig_duration) max_mig_duration = mig_duration;
/*    
    DVIC{
        if(tmp_stage2_done){
	    all_io_cnt = tlc_io_profile_report();
        }

        if(vic_flag){
            sprintf(progress_string, "s222 %" PRId64 " %" PRId64 " %" PRId64 " %.2lf", pages_gen_last_epoch, mig_duration, all_io_cnt, stage2_progress);
	    vic_report(progress_string);
        }
	//printf("s222 %" PRId64 " %" PRId64 " %" PRId64 " %.2lf\n", pages_gen_last_epoch, mig_duration, all_io_cnt, stage2_progress);
        //fflush(stdout);
    }
*/
}

QEMUFile *tlc_qemu_fopen_ops_buffered(void *opaque,
                                  size_t bytes_per_sec,
                                  BufferedPutFunc *put_buffer,
                                  BufferedPutReadyFunc *put_ready,
                                  BufferedWaitForUnfreezeFunc *wait_for_unfreeze,
                                  BufferedCloseFunc *close)
{
    QEMUFileBuffered *s;

    s = g_malloc0(sizeof(*s));

    s->opaque = opaque;
    s->xfer_limit = bytes_per_sec / 10;
    s->put_buffer = put_buffer;
    s->put_ready = put_ready;
    s->wait_for_unfreeze = wait_for_unfreeze;
    s->close = close;

    s->file = qemu_fopen_ops(s, buffered_put_buffer, NULL,
                             buffered_close, buffered_rate_limit,
                             buffered_set_rate_limit,
			     buffered_get_rate_limit);

    if(unlikely(mthread)){
    	s->timer = qemu_new_timer_ms(rt_clock, buffered_rate_tick_m, s);
    }
    else{
    	s->timer = qemu_new_timer_ms(rt_clock, buffered_rate_tick, s);
    }

    //qemu_mod_timer(s->timer, qemu_get_clock_ms(rt_clock) + DEFAULT_TLC_INTERVAL);
    qemu_mod_timer(s->timer, qemu_get_clock_ms(rt_clock) + tlc_interval);

    return s->file;
}

