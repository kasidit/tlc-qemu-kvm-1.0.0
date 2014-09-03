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

#include <sched.h>
// TLC profiling
typedef struct tlc_profile_node {
    //uint64_t basic_io_cnt;
    //uint64_t mmio_cnt;
    uint64_t total_io_cnt;
    int	nice_value; 
    //cpu_set_t cpu_mapping; 
} tlc_profile_type;

// VIC stuffs
#define	VIC_CONCURRENT_PROGRESS	"ct"
#define	VIC_STOP_TRANS_PROGRESS	"st"
#define	VIC_MESSAGE		"ms"

#define VIC_STR_LEN		100

// TLC Config
#define TLC_CONFIG_LINE_LEN	256
#define TLC_STR_LEN		128
#define TLC_CONFIG_FILE		"/tmp/tlc_params.conf"

// State Transfer Types
#define NOTYPE			0
#define TLC_EXEC		1
#define TLC_TCP_STANDBY_DST	2
#define TLM			3
#define TLM_STANDBY_DST		4
#define TLM_STANDBY_SRCDST	5
#define TLMC			6
#define TLMC_STANDBY_DST	7
#define TLM_EXEC		8
// TLM mode
#define TLM_NO_MODE		0
#define TLM_ONEPASS_MODE	1

// chkpt memory server def
#define MAX_TLC_MSERV		25
#define CHKPT_NAME_LEN		TLC_STR_LEN
#define CHKPT_MSG_LEN           5
#define CHKPT_CMD_LEN		4
#define CHKPT_ACK_LEN           3
#define INST_STR_LEN            256

// chkpt state
#define TLC_CHKPT_INIT		0
#define TLC_CHKPT_READY		1
#define TLC_CHKPT_ABORT		-7
#define TLC_CHKPT_COMMIT	2

// chkpt memory distribution
#define TLC_MEM_ADDR_CYCLIC	1
#define TLC_MEM_ADDR_BLOCK	2
#define TLC_MEM_ADDR_CYCLIC_DOUBLE_CHANNELS	3
#define TLC_MEM_ADDR_BLOCK_DOUBLE_CHANNELS	4
