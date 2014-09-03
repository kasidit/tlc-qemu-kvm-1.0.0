struct ram_node {
    uint8_t *page;
};
typedef struct ram_node ram_node_type; 

typedef uint64_t ram_addr_t;

#define 	ONEGIGABYTES			(1024 * 1024 * 1024)
#define 	ONEMEGABYTES			(1024 * 1024)

#define 	TARGET_PAGE_BITS 12
#define 	TARGET_PAGE_SIZE (1 << TARGET_PAGE_BITS)
#define 	TARGET_PAGE_MASK ~(TARGET_PAGE_SIZE - 1)

#define		MC_SERV_PORT	17700
#define		MC_SERV_IP_ADDR	"127.0.0.1"

#define 	TLC_EOF_MARKER	0x0FF
#define 	BACKLOG		5

#define 	CHKPT_CMD_LEN	4
#define 	CHKPT_NAME_LEN	128
#define 	INST_STR_LEN    256

// TLC VIC Stuffs 
#define		MC_VIC_PORT	17777
#define		MC_VIC_IP_ADDR	"127.0.0.1"
#define 	VIC_STR_LEN	100

#define		VIC_CONCURRENT_PROGRESS	"ct"
#define		VIC_STOP_TRANS_PROGRESS	"st"
#define		VIC_MESSAGE		"ms"

// TLC Config
#define 	TLC_CONFIG_LINE_LEN	256
#define 	TLC_STR_LEN		100
#define 	TLC_CONFIG_FILE		"/tmp/tlc_params.conf"

// State Transfer Types
#define 	NOTYPE			0
#define 	TLC_EXEC		1
#define 	TLC_TCP_STANDBY_DST	2
#define 	TLM			3
#define 	TLM_STANDBY_DST		4
#define 	TLM_STANDBY_SRCDST	5
#define 	TLMC			6
#define 	TLMC_STANDBY_DST	7
