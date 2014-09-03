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

#include "qemu-common.h"
#include "hw/hw.h"
#include "qemu-timer.h"
#include "qemu-char.h"
#include "buffered_file.h"

//#include "tlc-debug.h"

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

static void buffered_append(QEMUFileBuffered *s,
                            const uint8_t *buf, size_t size)
{
}

static void buffered_flush(QEMUFileBuffered *s)
{
}

static int buffered_put_buffer(void *opaque, const uint8_t *buf, int64_t pos, int size)
{
    return 0;
}

static int buffered_close(void *opaque)
{
    return 0;
}

/*
 * The meaning of the return values is:
 *   0: We can continue sending
 *   1: Time to stop
 *   negative: There has been an error
 */
static int buffered_rate_limit(void *opaque)
{
    return 0;
}

static int64_t buffered_set_rate_limit(void *opaque, int64_t new_rate)
{
    return 0;
}

static int64_t buffered_get_rate_limit(void *opaque)
{
    return 0;
}

static void buffered_rate_tick(void *opaque)
{
}

QEMUFile *qemu_fopen_ops_buffered(void *opaque,
                                  size_t bytes_per_sec,
                                  BufferedPutFunc *put_buffer,
                                  BufferedPutReadyFunc *put_ready,
                                  BufferedWaitForUnfreezeFunc *wait_for_unfreeze,
                                  BufferedCloseFunc *close)
{
// dummy functions & dummy calls
buffered_append(NULL, NULL, 0);
buffered_flush(NULL);
buffered_put_buffer(opaque, NULL, 0, 0);
buffered_close(opaque);
buffered_rate_limit(opaque);
buffered_set_rate_limit(opaque, 0);
buffered_get_rate_limit(opaque);
buffered_rate_tick(opaque);
    return NULL;
}

