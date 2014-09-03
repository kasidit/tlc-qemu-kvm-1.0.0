/*  This program is a part of TLC extensions to kvm.
    Author: kasidit chanchio   
    Copyright 2012
*/
#include <stdlib.h>
#include <stdio.h>

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "tlc.h"

void *tlc_incoming_t(void *t);
int accept_cr(int fd, struct sockaddr *addr, socklen_t *len);
int mc_set(const void *mkey, size_t mkey_size, const void *buf, size_t buf_size);
int mc_set2(const void *mkey, size_t mkey_size, const void *buf, size_t buf_size);
void mc_close(void);
void mc_close2(void);
int write_full(int fd, const void *buf, size_t count);
int read_full(int fd, void *buf, size_t count);

// TLC client-side functions
int mc_create_connection(int num_conn);
int mc_create_connection2(int num_conn);
int connect_w(int fd, const struct sockaddr *addr, socklen_t len);

extern 		int 		tlc_mserv_base_port;
extern		const char	*tlc_mserv_ip;  

#define		MC_SERV_PORT	13320
#define		MC_SERV_PORT2	MC_SERV_PORT+1

#define		MC_SERV_IP_ADDR	"127.0.0.1"
#define 	MC_DATA_LENGTH	4096

int select_w(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
// socket structure definitions 

int 	mclient_fd; 
struct 	sockaddr_in mc_serv_addr;

int 	mclient_fd2; 
struct 	sockaddr_in mc_serv_addr2;

// data structures for batch sending 
//fd_set  mc_rset;
//fd_set  mc_wset;
//int     mc_maxfd;

// TLC client-side function
int mc_create_connection(int num_conn){
	int ret;

	mclient_fd = socket(AF_INET, SOCK_STREAM, 0); 

	mc_serv_addr.sin_family = AF_INET;
	
	if(tlc_mserv_base_port != 0){
printf(" TLC base port = %d\n", tlc_mserv_base_port); 
		mc_serv_addr.sin_port = htons(tlc_mserv_base_port);
	}
	else{
		mc_serv_addr.sin_port = htons(MC_SERV_PORT);
	}
	
	if(tlc_mserv_ip != NULL){
printf(" TLC mserv ip = %s\n", tlc_mserv_ip); 
		mc_serv_addr.sin_addr.s_addr = inet_addr(tlc_mserv_ip);
	}
	else{
		mc_serv_addr.sin_addr.s_addr = inet_addr(MC_SERV_IP_ADDR);
	}

	if((ret = connect_w(mclient_fd, (struct sockaddr *) &mc_serv_addr, sizeof(mc_serv_addr))) < 0){
		printf("Connect error 1 occured errno = %d\n", errno);
		exit(1);
	}
	return ret;
}

int mc_create_connection2(int num_conn){
	int ret;

	mclient_fd2 = socket(AF_INET, SOCK_STREAM, 0); 

	mc_serv_addr2.sin_family = AF_INET;
	
	if(tlc_mserv_base_port != 0){
//printf(" conn 2 TLC base port = %d\n", tlc_mserv_base_port); 
		mc_serv_addr2.sin_port = htons(tlc_mserv_base_port+1);
	}
	else{
		mc_serv_addr2.sin_port = htons(MC_SERV_PORT2);
	}
	
	if(tlc_mserv_ip != NULL){
//printf(" conn 2 TLC mserv ip = %s\n", tlc_mserv_ip); 
		mc_serv_addr2.sin_addr.s_addr = inet_addr(tlc_mserv_ip);
	}
	else{
		mc_serv_addr2.sin_addr.s_addr = inet_addr(MC_SERV_IP_ADDR);
	}

	if((ret = connect_w(mclient_fd2, (struct sockaddr *) &mc_serv_addr2, sizeof(mc_serv_addr2))) < 0){
		printf("Connect error 2 occured errno = %d\n", errno);
		exit(1);
	}
	return ret;
}

int 	 chkpt_mserv_client_fd[MAX_TLC_MSERV]; 
struct 	 sockaddr_in chkpt_mserv_addr[MAX_TLC_MSERV];
uint64_t mserv_target_page_bits = 0;

extern int  chkpt_mserv_cnt;
extern char *tlc_chkpt_mserv_ip[MAX_TLC_MSERV];
extern int  tlc_chkpt_mserv_port[MAX_TLC_MSERV];
extern char *tlc_chkpt_name_str;
extern char chkpt_recovery_inst_str[MAX_TLC_MSERV][INST_STR_LEN];
extern uint64_t mserv_base_page_id[MAX_TLC_MSERV];
extern uint64_t mserv_allocate_pages[MAX_TLC_MSERV];

extern uint64_t tlc_target_page_bits(void);
extern uint64_t tlc_target_page_size(void);

int mserv_init_command(
              int client_fd,
              char *cmd, 
              uint64_t base_page_id, 
              uint64_t num_pages_allocated, 
              char *checkpoint_name);

int mserv_create_connections(void);

int mserv_create_connections(void){
    char mserv_cmd[CHKPT_CMD_LEN];
    int i, ret = TLC_CHKPT_READY;

    for(i = 0; i < chkpt_mserv_cnt; i++){

	chkpt_mserv_client_fd[i] = socket(AF_INET, SOCK_STREAM, 0); 

	chkpt_mserv_addr[i].sin_family = AF_INET;
	
	if(tlc_chkpt_mserv_port[i] != 0){
printf(" TLC chkpt base port = %d\n", tlc_chkpt_mserv_port[i]); 
		chkpt_mserv_addr[i].sin_port = htons(tlc_chkpt_mserv_port[i]);
	}
	else{
		chkpt_mserv_addr[i].sin_port = htons(MC_SERV_PORT);
	}
	
	if(tlc_chkpt_mserv_ip[i] != NULL){
printf(" TLC mserv ip = %s\n", tlc_chkpt_mserv_ip[i]); 
		chkpt_mserv_addr[i].sin_addr.s_addr = inet_addr(tlc_chkpt_mserv_ip[i]);
	}
	else{
		chkpt_mserv_addr[i].sin_addr.s_addr = inet_addr(MC_SERV_IP_ADDR);
	}

printf("size of %lu compared with %lu\n", sizeof(chkpt_mserv_addr[i]), sizeof(struct sockaddr_in));

	if((ret = connect_w(chkpt_mserv_client_fd[i], (struct sockaddr *) &(chkpt_mserv_addr[i]), sizeof(chkpt_mserv_addr[i]))) < 0){
		printf("mserv_create_connection: Connect %d error 1 occured errno = %d\n", i, errno);
                chkpt_mserv_client_fd[i] = -1;
                ret = TLC_CHKPT_ABORT;
		break;
	}

        memset(mserv_cmd, 0, CHKPT_CMD_LEN);
        strncpy(mserv_cmd, "PUT", 3);

        mserv_init_command(
              chkpt_mserv_client_fd[i],
              mserv_cmd, 
              mserv_base_page_id[i], 
              mserv_allocate_pages[i], 
              tlc_chkpt_name_str
              );
     
    }
        // create recovery instruction to be sent to TLC dst later 
/*
        memset(chkpt_recovery_inst_str[i], 0, INST_STR_LEN);

        snprintf(
              chkpt_recovery_inst_str[i], INST_STR_LEN,
              "%s:%d:%" PRId64 ":%" PRId64 ":%s", 
              tlc_chkpt_mserv_ip[i],
              tlc_chkpt_mserv_port[i],
              mserv_base_page_id[i], 
              mserv_allocate_pages[i], 
              tlc_chkpt_name_str);
 
printf(" mserv cmd = %s my chkpt name = %s recover instr = %s\n", mserv_cmd, tlc_chkpt_name_str, chkpt_recovery_inst_str[i]);
*/
    if(ret == TLC_CHKPT_READY){ 
        mserv_target_page_bits = tlc_target_page_bits();
    }
    else mserv_target_page_bits = -1; // used (temporarily) by assertion in mserv_set

    return ret;
}

int mserv_init_command(
              int client_fd,
              char *cmd, 
              uint64_t base_page_id, 
              uint64_t num_pages_allocated, 
              char *checkpoint_name){
    int ret = -1; 
    uint64_t tmp_target_page_bits;
    uint64_t tmp_target_page_size;

    if((ret = write_full(client_fd, cmd, CHKPT_CMD_LEN)) < 0){
        printf(" mserv_init_set: Error: Unsuccessful cmd write: %s\n", cmd);
    	return ret;
    }

    tmp_target_page_bits = tlc_target_page_bits();
    if((ret = write_full(client_fd, &tmp_target_page_bits, sizeof(uint64_t))) < 0){
        printf(" merv_init_set: Error: Unsuccessful pagebits write %" PRIx64 "\n", tmp_target_page_bits);
    	return ret;
    }

    tmp_target_page_size = tlc_target_page_size();
    if((ret = write_full(client_fd, &tmp_target_page_size, sizeof(uint64_t))) < 0){
        printf(" mserv_init_set: Error: Unsuccessful pagesize write %" PRIx64 "\n", tmp_target_page_size);
    	return ret;
    }

    if((ret = write_full(client_fd, &base_page_id, sizeof(uint64_t))) < 0){
        printf(" mserv_init_set: Error: Unsuccessful basepageid write %" PRIx64 "\n", base_page_id);
    	return ret;
    }

    if((ret = write_full(client_fd, &num_pages_allocated, sizeof(uint64_t))) < 0){
        printf(" mserv_init_set: Error: Unsuccessful numpagealloc write %" PRIx64 "\n", num_pages_allocated);
    	return ret;
    }

    if((ret = write_full(client_fd, checkpoint_name, CHKPT_NAME_LEN)) < 0){
        printf(" mserv_init_set: Error: Unsuccessful chkptname write: %s\n", checkpoint_name);
    	return ret;
    }

    return ret;
}

int connect_w(int fd, const struct sockaddr *addr, socklen_t len){
	int ret;
repeat_connect:
        ret = connect(fd, addr, len);
        if (ret < 0) {
            if (errno == EINTR){
                goto repeat_connect;
	    }
	    printf("connect error errno=%d fd=%d\n", errno, fd);
        }
	return ret;

}

int mc_set(const void *mkey, size_t mkey_size, const void *buf, size_t buf_size){
    int ret = -1;
    
//    printf(" mc_set: try write 1 key = %" PRIx64 " \n", (uint64_t) mkey);
//   fflush(stdout);
    if((ret = write_full(mclient_fd, mkey, mkey_size)) < 0){
        printf(" mc_set: Error: Unsuccessful key write %" PRIx64 "\n", (uint64_t) mkey);
    	return ret;
    }
//    printf(" mc_set: first write %d ret = %d\n", mclient_fd, ret);
    if((ret = write_full(mclient_fd, buf, buf_size)) < 0){
        printf(" mc_set: Error: Unsuccessful page write (key = %" PRIx64 ") \n", (uint64_t) mkey);
        return ret;
    }
//    printf(" mc_set: second write ret = %d\n", ret);
    return ret;   
}

int mc_set2(const void *mkey, size_t mkey_size, const void *buf, size_t buf_size){
    int ret = -1;
    
    if((ret = write_full(mclient_fd2, mkey, mkey_size)) < 0){
        printf(" mc_set2: Error: Unsuccessful key write %" PRIx64 "\n", (uint64_t) mkey);
    	return ret;
    }
    //printf(" mc_set2: first write %d ret = %d\n", mclient_fd, ret);
    if((ret = write_full(mclient_fd2, buf, buf_size)) < 0){
        printf(" mc_set2: Error: Unsuccessful page write (key = %" PRIx64 ") \n", (uint64_t) mkey);
        return ret;
    }
    //printf(" mc_set2: second write ret = %d\n", ret);
    return ret;   
}

#define TLC_EOF_MARKER	0x0FF

void mc_close(void){
    uint64_t eof_flag = TLC_EOF_MARKER;
    
    if(write_full(mclient_fd, &eof_flag, sizeof(uint64_t)) < 0){
        printf(" mc_close: Error: Unsuccessful EOF key write %" PRIx64 "\n", (uint64_t) eof_flag);
    }
    //printf(" mc_close: write eof\n");
    close(mclient_fd);
    //shutdown(mclient_fd, SHUT_WR);
}

void mc_close2(void){
    uint64_t eof_flag = TLC_EOF_MARKER;
    
    if(write_full(mclient_fd2, &eof_flag, sizeof(uint64_t)) < 0){
        printf(" mc_close2: Error: Unsuccessful EOF key write %" PRIx64 "\n", (uint64_t) eof_flag);
    }
    //printf(" mc_close: write eof\n");
    close(mclient_fd2);
    //shutdown(mclient_fd, SHUT_WR);
}

uint64_t mserv_set_item_cnt = 0; // debugging
extern uint64_t per_mserv_allocate_pages;

int mserv_set(const void *mkey, size_t mkey_size, const void *buf, size_t buf_size){
    int ret = -1;
    
    uint64_t addr = *((uint64_t *)mkey);
    uint64_t page_id;
    // determine which server to send to based on block distribution
    uint64_t mserv_id; 
    int fd = -1;

    if(mserv_set_item_cnt % (1024*4096) == 0){ // for debugging 
        printf(" mserv_set: write 1 ptr = %x key = %" PRId64 " \n", (uint64_t *)mkey, *((uint64_t *)mkey));
        fflush(stdout);
    }

    if(mserv_target_page_bits > 0){ // this one get set when all mservs are ok.
        page_id = addr >> mserv_target_page_bits;
        mserv_id = page_id/per_mserv_allocate_pages; 
    }
    else{
        printf("mserv_set: assertion! error mserv_target_page_bits = %d\n",
          mserv_target_page_bits);
        exit(1);
    }

    if (mserv_id < chkpt_mserv_cnt){ 
        fd = chkpt_mserv_client_fd[mserv_id];
    }
    else{
        printf(" mserv_set: Abort! error on  ptr = %x key (addr) = %" PRId64 " page id = %" PRId64 "\n", (uint64_t *)mkey, *((uint64_t *)mkey), page_id);
        printf(" mserv_set: mserv_id = %" PRId64 " chkpt_cnt = %d per_mserv = %" PRId64 " sent_item %" PRId64  "\n", mserv_id, chkpt_mserv_cnt, per_mserv_allocate_pages, mserv_set_item_cnt);
        fflush(stdout);
        exit(1);
    }
 
    mserv_set_item_cnt++;

    if((ret = write_full(fd, mkey, mkey_size)) < 0){
        printf(" mserv_set: Error: Unsuccessful key write %" PRIx64 "\n", (uint64_t) mkey);
    	return ret;
    }
//    printf(" mc_set: first write %d ret = %d\n", mclient_fd, ret);
    if((ret = write_full(fd, buf, buf_size)) < 0){
        printf(" mserv_set: Error: Unsuccessful value write (key = %" PRIx64 ") \n", (uint64_t) mkey);
        return ret;
    }
//    printf(" mc_set: second write ret = %d\n", ret);
    return ret;   
}

void mserv_close(void);

void mserv_close(void){
    int i;
    uint64_t eof_flag = TLC_EOF_MARKER;
    uint64_t ret_eof_flag;
    
    for(i = 0; i < chkpt_mserv_cnt; i++){
        if(write_full(chkpt_mserv_client_fd[i], &eof_flag, sizeof(uint64_t)) < 0){
          printf(" mserv_close: Error: Unsuccessful EOF key write %" PRIx64 "\n", (uint64_t) eof_flag);
        }
        if(read_full(chkpt_mserv_client_fd[i], &ret_eof_flag, sizeof(uint64_t)) < 0){
          printf(" mserv_close: Unsuccessful EOF key read back %" PRIx64 "\n", (uint64_t) ret_eof_flag);
        }
    //printf(" mc_close: write eof\n");
        close(chkpt_mserv_client_fd[i]);
    }
}

// TLC server-side functions
extern int  state_transfer_type;
extern void tlc_parallel_receiving_pages(int fd);
extern void tlc_parallel_receiving_pages2(int fd);
extern void tlc_barrier_synchronization_with_io_thr(void);

#define 	BACKLOG		5

// socket structure definitions 
int mserv_lis_fd;
int mserv_conn_fd; 
struct sockaddr_in mserv_addr;

int mserv_lis_fd2;
int mserv_conn_fd2; 
struct sockaddr_in mserv_addr2;

void *tlc_incoming_t(void *t){

    if(
      (state_transfer_type == TLM)||
      (state_transfer_type == TLM_STANDBY_DST)||
      (state_transfer_type == TLM_STANDBY_SRCDST)||
      (state_transfer_type == TLMC)||
      (state_transfer_type == TLMC_STANDBY_DST)
      ){
	mserv_lis_fd = socket(AF_INET, SOCK_STREAM, 0); 

	memset(&mserv_addr, 0, sizeof(mserv_addr));
	mserv_addr.sin_family = AF_INET;
	
	if(tlc_mserv_base_port != 0){
		mserv_addr.sin_port = htons(tlc_mserv_base_port);
	}
	else{
		mserv_addr.sin_port = htons(MC_SERV_PORT);
	}
	
	mserv_addr.sin_addr.s_addr = INADDR_ANY;

	bind(mserv_lis_fd, (struct sockaddr *) &mserv_addr, sizeof(mserv_addr)); 

	listen(mserv_lis_fd, BACKLOG);

// conn2
	mserv_lis_fd2 = socket(AF_INET, SOCK_STREAM, 0); 

	memset(&mserv_addr2, 0, sizeof(mserv_addr2));
	mserv_addr2.sin_family = AF_INET;

	if(tlc_mserv_base_port != 0){
		mserv_addr2.sin_port = htons(tlc_mserv_base_port+1);
	}
	else{
		mserv_addr2.sin_port = htons(MC_SERV_PORT2);
	}

	mserv_addr2.sin_addr.s_addr = INADDR_ANY;

	bind(mserv_lis_fd2, (struct sockaddr *) &mserv_addr2, sizeof(mserv_addr2)); 

	listen(mserv_lis_fd2, BACKLOG);

//printf("tlc incoming thr running x = %d\n", (int) x);
//fflush(stdout);
	if((mserv_conn_fd = accept_cr(mserv_lis_fd, NULL, NULL)) < 0){
		printf("Accept: Error occured\n");
		exit(1);
	}
//printf("tlc thr connection 1 was made on the server\n");
//fflush(stdout);

	if((mserv_conn_fd2 = accept_cr(mserv_lis_fd2, NULL, NULL)) < 0){
		printf("Accept: Error occured\n");
		exit(1);
	}
//printf("tlc thr connection 2 was made on the server\n");
//fflush(stdout);

// recv conn 1	
//printf("receiving from  connection 1\n");
//fflush(stdout);
	tlc_parallel_receiving_pages(mserv_conn_fd);
	
	close(mserv_conn_fd);
	close(mserv_lis_fd);

//printf("receiving from connection 2\n");
//fflush(stdout);
	tlc_parallel_receiving_pages2(mserv_conn_fd2);
	
	close(mserv_conn_fd2);
	close(mserv_lis_fd2);

        tlc_barrier_synchronization_with_io_thread();

    }
    else if(
      (state_transfer_type == TLC_EXEC)||
      (state_transfer_type == TLC_TCP_STANDBY_DST)
    ){
        printf("tlc_incoming_t: invalid TLC state transfer type  = %d \n", state_transfer_type);
        t = -1;
    }
    else{
        printf("tlc_incoming_t: unknown state transfer type  = %d \n", state_transfer_type);
        t = -1;
    }

    pthread_exit((void *) t);	
}

int accept_cr(int fd, struct sockaddr *addr, socklen_t *len){
	int ret;
repeat_accept:
        ret = accept(fd, addr, len);
        if (ret < 0) {
            if (errno == EINTR){
                goto repeat_accept;
	    }
	    printf("accept error errno=%d fd=%d\n", errno, fd);
        }
	return ret;
}

int select_w(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout){
	int ret;
repeat_select:
	ret = select(nfds, readfds, writefds, exceptfds, timeout);
        if (ret < 0) {
            if (errno == EINTR){
                goto repeat_select;
	    }
	    printf("select error errno=%d \n", errno);
        }
	return ret;
}

int write_full(int fd, const void *buf, size_t count){
    ssize_t ret = 0;
    ssize_t total = 0;

    while (count) {
        ret = write(fd, buf, count);
        if (ret < 0) {
            if (errno == EINTR){
                continue;
	    }
	    printf("write error errno=%d fd=%d\n", errno, fd);
            return ret;
        }

        count -= ret;
        buf += ret;
        total += ret;
    }

    return total;
}

int read_full(int fd, void *buf, size_t count){
    ssize_t ret = 0;
    ssize_t total = 0;

    while (count) {
        ret = read(fd, buf, count);
        if (ret < 0) {
            if (errno == EINTR){ 
                continue;
	    } 
	    printf("read error errno=%d fd=%d\n", errno, fd);
            return ret;
        }

        count -= ret;
        buf += ret;
        total += ret;
    }

    return total;
}
// TLC VIC  
#define		MC_VIC_PORT	17777
#define		MC_VIC_IP_ADDR	"127.0.0.1"

extern int		vic_flag;
extern int 		vic_port;
extern const char 	*vic_ip;

int 	vic_fd; 
struct 	sockaddr_in vic_addr;

int vic_create_connection(int num_conn);

int vic_create_connection(int num_conn){
        char vic_message[150];
	int ret;

	vic_fd = socket(AF_INET, SOCK_STREAM, 0); 

	vic_addr.sin_family = AF_INET;
	
	if(vic_port != 0){
printf(" VIC conn vic port = %d\n", vic_port); 
		vic_addr.sin_port = htons(vic_port);
	}
	else{
		vic_addr.sin_port = htons(MC_VIC_PORT);
	}
	
	if(vic_ip != NULL){
printf(" VIC conn vic ip = %s\n", vic_ip); 
		vic_addr.sin_addr.s_addr = inet_addr(vic_ip);
	}
	else{
		vic_addr.sin_addr.s_addr = inet_addr(MC_VIC_IP_ADDR);
	}

	if((ret = connect_w(vic_fd, (struct sockaddr *) &vic_addr, sizeof(vic_addr))) < 0){
		printf("VIC Connect error occured errno = %d\n", errno);
		//exit(1); 
		vic_flag = 0;
	}
        // register to vic server

        if(vic_flag == 1){
                strcpy(vic_message, "vmRegis vm001\0");

        	printf(" vic_connect: sending a message %s\n", vic_message);
        	if((ret = write_full(vic_fd, vic_message, strlen(vic_message))) < 0){
            		printf(" vic_connect: Error: fail sending a message %s\n", vic_message);
			vic_flag = 0;
        	}
	}
        //if((ret = read_full(vic_fd, vic_message, sizeof(vic_message))) < 0){
        //    printf(" vic_connect: Error: fail receiving a return message %s\n", vic_message);
        //}
        //printf(" vic_connect: receive a message %s\n", vic_message);

	return ret;
}

int vic_report(char *message);

int vic_report(char *message){
    int ret; 
    if((ret = write_full(vic_fd, message, strlen(message))) < 0){
        printf(" vic_report: Error: fail sending a message %s\n", message);
    }
    return ret;
}

