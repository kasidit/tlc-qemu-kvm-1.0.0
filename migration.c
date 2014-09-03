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

//#define MAX_THROTTLE  (32 << 20)      

NotifierList migration_state_notifiers =
    NOTIFIER_LIST_INITIALIZER(migration_state_notifiers);
/*
// TLC declarations
#include 	<pthread.h>
#include 	"tlc-debug.h"

int64_t 	mig_start, mig_finish;
int64_t 	mig_period_start, mig_period_end, 
		mig_duration, acc_vm_overhead, max_mig_duration;
int 		duration_id;
extern int64_t 	stage_start, stage_finish;

int64_t 	epoch_start, epoch_end, epoch_duration;
uint64_t 	pages_copied_last_epoch; 
uint64_t 	pages_gen_last_epoch;

extern uint64_t tlc_s21_pages_transferred;
extern uint64_t tlc_s21_bytes_transferred;

int 		ncpus;
extern int	mthread;

extern uint64_t	do_migrate_id;
extern uint64_t	chkp_thread_id;

extern int	idone;

extern pthread_mutex_t vm_waitfor_checkpoint_thread;
extern pthread_mutex_t mutex_idone;
extern pthread_cond_t  save_dev_state_done;
extern int 	save_dev_state; 

pthread_rwlock_t dirty_page_rwlock;

extern uint64_t tlc_s22_pages_transferred;
extern uint64_t tlc_s22_bytes_transferred;

// TLC: begin
extern void tlc_migration_init(int flag);
extern void tlc_migration_finish(int flag);
extern void *page_saver_t(void *args);
//int final_migration_cleanup(MigrationState *s);
// TLC: end
extern int vic_flag;
extern int vic_report(char *message);

extern int state_transfer_type; 
*/
//extern MigrationState *tlc_migrate_get_current(void);
/*
static MigrationState *migrate_get_current(void)
{
    return tlc_migrate_get_current();
}
*/

extern int tlc_qemu_start_incoming_migration(const char *uri);

int qemu_start_incoming_migration(const char *uri)
{
    return tlc_qemu_start_incoming_migration(uri);
}
void tlc_process_incoming_migration(QEMUFile *f);

void process_incoming_migration(QEMUFile *f)
{
    tlc_process_incoming_migration(f);
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

extern MigrationInfo *tlc_qmp_query_migrate(Error **errp);

MigrationInfo *qmp_query_migrate(Error **errp)
{
    return tlc_qmp_query_migrate(errp);
}
extern void tlc_migrate_fd_error(MigrationState *s);

void migrate_fd_error(MigrationState *s)
{
    tlc_migrate_fd_error(s);
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
extern int tlc_pages_sent_mserv; 
extern MigrationState *migrate_init(Monitor *mon, int detach, int blk, int inc);
static GSList *migration_blockers;

void migrate_add_blocker(Error *reason)
{
    migration_blockers = g_slist_prepend(migration_blockers, reason);
}

void migrate_del_blocker(Error *reason)
{
    migration_blockers = g_slist_remove(migration_blockers, reason);
}

extern int tlc_do_migrate(Monitor *mon, int detach, int blk, int inc, const char *uri);

int do_migrate(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    int detach = qdict_get_try_bool(qdict, "detach", 0);
    int blk = qdict_get_try_bool(qdict, "blk", 0);
    int inc = qdict_get_try_bool(qdict, "inc", 0);
    const char *uri = qdict_get_str(qdict, "uri");

    if (qemu_savevm_state_blocked(mon)) {
        return -1;
    }

    if (migration_blockers) {
        Error *err = migration_blockers->data;
        qerror_report_err(err);
        return -1;
    }

    if(tlc_do_migrate(mon, detach, blk, inc, uri) < 0){
        return -1;
    }  

    return 0;
}
extern void tlc_do_migrate_cancel(void);

int do_migrate_cancel(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    tlc_do_migrate_cancel();
    return 0;
}

// TLC migration configuration parameter strings
extern int tlc_do_config_tlc(char *filename);

int do_config_tlc(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    int ret;

    const char *filename = qdict_get_str(qdict, "filename");

    ret = tlc_do_config_tlc((char *)filename); // parse config file
    return ret;
}

extern void tlc_do_migrate_set_speed(int64_t d);
extern void tlc_do_migrate_set_downtime(int64_t d);

int do_migrate_set_speed(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    int64_t d;
    //MigrationState *s;

    d = qdict_get_int(qdict, "value");
    if (d < 0) {
        d = 2;
    }
//printf(" d = %d \n", d);
//fflush(stdout);
    tlc_do_migrate_set_speed(d); // don't use by TLC may remove later

    return 0;
}

int do_migrate_set_downtime(Monitor *mon, const QDict *qdict,
                            QObject **ret_data)
{
    double d;

    d = qdict_get_double(qdict, "value") * 1e9;
    d = MAX(0, MIN(UINT64_MAX, d));
    max_downtime = (uint64_t)d;

    tlc_do_migrate_set_downtime(d); // don't use by TLC may remove later

    return 0;
}
