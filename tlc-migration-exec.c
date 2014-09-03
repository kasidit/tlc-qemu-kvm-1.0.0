/*
 * QEMU live migration
 *
 * Copyright IBM, Corp. 2008
 * Copyright Dell MessageOne 2008
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *  Charles Duffy     <charles_duffy@messageone.com>
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
#include "qemu_socket.h"
#include "migration.h"
#include "qemu-char.h"
#include "buffered_file.h"
#include "block.h"
#include <sys/types.h>
#include <sys/wait.h>

// TLC
extern 	int mthread;
//extern void tlc_process_incoming_migration(QEMUFile *f);

//#define DEBUG_MIGRATION_EXEC

#ifdef DEBUG_MIGRATION_EXEC
#define DPRINTF(fmt, ...) \
    do { printf("migration-exec: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

static int file_errno(MigrationState *s)
{
    return errno;
}

static int file_write(MigrationState *s, const void * buf, size_t size)
{
    return write(s->fd, buf, size);
}

static int exec_close(MigrationState *s)
{
    int ret = 0;
    DPRINTF("exec_close\n");
    if (s->opaque) {
        ret = qemu_fclose(s->opaque);
        s->opaque = NULL;
        s->fd = -1;
        if (ret != -1 &&
            WIFEXITED(ret)
            && WEXITSTATUS(ret) == 0) {
            ret = 0;
        } else {
            ret = -1;
        }
    }
    return ret;
}

#include "qemu-timer.h"
//#include "tlc-debug.h"

int tlc_exec_start_outgoing_migration(MigrationState *s, const char *command)
{
    FILE *f;

//int64_t test_timer_start, 
//	test_timer_stop3,
//	test_timer_stop2,
//	test_timer_stop1;

//test_timer_start = qemu_get_clock_ms(rt_clock);

    f = popen(command, "w");
    if (f == NULL) {
        DPRINTF("Unable to popen exec target\n");
        goto err_after_popen;
    }
//test_timer_stop1 = qemu_get_clock_ms(rt_clock);
//DREG{printf("exec: test_timer1: elasp =  %" PRId64 " ms\n", test_timer_stop1 - test_timer_start);
//fflush(stdout);}

    s->fd = fileno(f);
    if (s->fd == -1) {
        DPRINTF("Unable to retrieve file descriptor for popen'd handle\n");
        goto err_after_open;
    }

    if(unlikely(mthread)){
	//printf("exec_outgoing: set blocking\n");
	fflush(stdout);
    	socket_set_block(s->fd);
    }
    else{
	printf("exec_outgoing: set NON blocking\n");
	fflush(stdout);
    	socket_set_nonblock(s->fd);
    }

    s->opaque = qemu_popen(f, "w");
//test_timer_stop2 = qemu_get_clock_ms(rt_clock);
//DREG{printf("exec: test_timer2: elasp =  %" PRId64 " ms\n", test_timer_stop2 - test_timer_stop1);
//fflush(stdout);}

    s->close = exec_close;
    s->get_error = file_errno;
    s->write = file_write;

    migrate_fd_connect(s);

//test_timer_stop3 = qemu_get_clock_ms(rt_clock);
//DREG{printf("exec: test_timer3: elasp =  %" PRId64 " ms\n", test_timer_stop3 - test_timer_stop2);
//fflush(stdout);}

    return 0;

err_after_open:
    pclose(f);
err_after_popen:
    return -1;
}

#define	TLC_OUTGOING_PAGES	1
#define TLC_INCOMING_PAGES	2

extern void tlc_migration_init(int flag);
extern void tlc_migration_finish(int flag);
extern void live_storage_init(int flag, int num_thr);

static void exec_accept_incoming_migration(void *opaque)
{
    QEMUFile *f = opaque;

    // TLC (move below to exec_start_incoming_migration()
printf(" in tlc_exec_accept_incoming_migration()\n");
    mthread = 1;
    //incoming_protocol = INCOMING_EXEC; // s'th to tell the vm not autostart
    tlc_migration_init(TLC_INCOMING_PAGES);

    process_incoming_migration(f);

    tlc_migration_finish(TLC_INCOMING_PAGES);
    mthread = 0;

    qemu_set_fd_handler2(qemu_stdio_fd(f), NULL, NULL, NULL, NULL);
    qemu_fclose(f);
}

int tlc_exec_start_incoming_migration(const char *command)
{
    QEMUFile *f;
    
    live_storage_init(TLC_INCOMING_PAGES, 1);

    DPRINTF("Attempting to start an incoming migration\n");
    f = qemu_popen_cmd(command, "r");
    if(f == NULL) {
        DPRINTF("Unable to apply qemu wrapper to popen file\n");
        return -errno;
    }

    qemu_set_fd_handler2(qemu_stdio_fd(f), NULL,
			 exec_accept_incoming_migration, NULL, f);

    return 0;
}
