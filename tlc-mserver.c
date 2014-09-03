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
#include "qemu-timer.h"

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

#define		MC_SERV_PORT	18800 // default tlm mserv port 
#define		MC_SERV_PORT2	MC_SERV_PORT+1

#define		MC_SERV_IP_ADDR	"127.0.0.1"
#define 	MC_DATA_LENGTH	4096

int select_w(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
// socket structure definitions 

int 	mclient_fd; 
struct 	sockaddr_in mc_serv_addr;

int 	mclient_fd2; 
struct 	sockaddr_in mc_serv_addr2;

// TLC client-side function
int mc_create_connection(int num_conn){
	int ret;

	mclient_fd = socket(AF_INET, SOCK_STREAM, 0); 

	mc_serv_addr.sin_family = AF_INET;
	
	if(tlc_mserv_base_port != 0){
//printf(" TLC base port = %d\n", tlc_mserv_base_port); 
		mc_serv_addr.sin_port = htons(tlc_mserv_base_port);
	}
	else{
		mc_serv_addr.sin_port = htons(MC_SERV_PORT);
	}
	
	if(tlc_mserv_ip != NULL){
//printf(" TLC mserv ip = %s\n", tlc_mserv_ip); 
		mc_serv_addr.sin_addr.s_addr = inet_addr(tlc_mserv_ip);
	}
	else{
		mc_serv_addr.sin_addr.s_addr = inet_addr(MC_SERV_IP_ADDR);
	}

	if((ret = connect_w(mclient_fd, (struct sockaddr *) &mc_serv_addr, sizeof(mc_serv_addr))) < 0){
		printf("mc_create_connection (1) error errno = %d\n", errno);
                fflush(stdout);
		//exit(1); // For Debugging Only
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
		printf("mc_create_connection2 (2) error errno = %d\n", errno);
                fflush(stdout);
		//exit(1); // For Debugging Only. Should fail more gracefully
	}
	return ret;
}

int 	 chkpt_mserv_client_fd[MAX_TLC_MSERV]; 
int 	 base_chkpt_mserv_client_fd[MAX_TLC_MSERV]; 
struct 	 sockaddr_in chkpt_mserv_addr[MAX_TLC_MSERV];
struct 	 sockaddr_in base_chkpt_mserv_addr[MAX_TLC_MSERV];
uint64_t mserv_target_page_bits = 0;

extern int  tlc_chkpt_type_flag; 
extern int  state_transfer_type;
extern int  chkpt_mserv_cnt;
extern char *tlc_chkpt_mserv_ip[MAX_TLC_MSERV];
extern int  tlc_chkpt_mserv_port[MAX_TLC_MSERV];
extern char *tlc_chkpt_name_str;
extern char chkpt_recovery_inst_str[MAX_TLC_MSERV][INST_STR_LEN];
extern uint64_t mserv_base_page_id[MAX_TLC_MSERV];
extern uint64_t mserv_allocate_pages[MAX_TLC_MSERV];

extern uint64_t tlc_target_page_bits(void);
extern uint64_t tlc_target_page_size(void);

int mserv_init_command_block(
              int client_fd,
              char *cmd, 
              uint64_t num_pages_allocated, 
              uint64_t base_page_id, 
              char *checkpoint_name);

int mserv_init_command_interleave(
              int client_fd,
              char *cmd, 
              uint64_t num_pages_allocated, 
              int  servno,
              char *checkpoint_name);

int mserv_get_command(
              int client_fd,
              char *cmd, 
              uint64_t num_pages_allocated, 
              int  servno,
              char *checkpoint_name);

int mserv_get_command_interleave(
              int client_fd,
              char *cmd, 
              uint64_t num_pages_allocated, 
              int  servno,
              char *checkpoint_name);

int mserv_create_connections(void);

int mserv_create_connections(void){
    char mserv_cmd[CHKPT_CMD_LEN];
    char ack_message[CHKPT_MSG_LEN];
    int n, i, ret = TLC_CHKPT_READY;

    if(
      (state_transfer_type == TLM)||
      (state_transfer_type == TLM_STANDBY_DST)||
      (state_transfer_type == TLM_STANDBY_SRCDST)||
      (state_transfer_type == TLMC)||
      (state_transfer_type == TLMC_STANDBY_DST)
      ){

        if(mc_create_connection(1) < 0){
            ret = TLC_CHKPT_ABORT;
            goto out_mserv_create_connection;
        }
        if(mc_create_connection2(1) < 0){
            ret = TLC_CHKPT_ABORT;
            goto out_mserv_create_connection;
        }
    }
    else if(
      (state_transfer_type == TLM_EXEC)||
      (state_transfer_type == TLC_EXEC)||
      (state_transfer_type == TLC_TCP_STANDBY_DST)
    ){
printf("mserv_create_connection: chkpt type flag = %d\n", tlc_chkpt_type_flag);
fflush(stdout);
      for(i = 0; i < chkpt_mserv_cnt; i++){
        int base_port; 
        if(
          (tlc_chkpt_type_flag == TLC_MEM_ADDR_CYCLIC_DOUBLE_CHANNELS) ||
          (tlc_chkpt_type_flag == TLC_MEM_ADDR_BLOCK_DOUBLE_CHANNELS)
          ){
printf("mserv_create_connection: DOUBLE CHANNALS, port \n");
fflush(stdout);
	  base_chkpt_mserv_client_fd[i] = socket(AF_INET, SOCK_STREAM, 0); 
	  base_chkpt_mserv_addr[i].sin_family = AF_INET;
          // base port = port + 100
printf(" TLC chkpt port[%d] = %d\n", i, tlc_chkpt_mserv_port[i]); 
fflush(stdout);
	  if(tlc_chkpt_mserv_port[i] != 0){
                base_port = tlc_chkpt_mserv_port[i]+100; 
printf(" 2 TLC chkpt base port = %d\n", base_port); 
fflush(stdout);
		base_chkpt_mserv_addr[i].sin_port = htons(base_port);
	  }
	  else{
		base_chkpt_mserv_addr[i].sin_port = htons(MC_SERV_PORT + 100);
	  }
printf(" TLC mserv ip[%d] = %s\n", i, tlc_chkpt_mserv_ip[i]); 
fflush(stdout);
	  if(tlc_chkpt_mserv_ip[i] != NULL){
printf(" 2 TLC mserv ip[%d] = %s\n", i, tlc_chkpt_mserv_ip[i]); 
fflush(stdout);
		base_chkpt_mserv_addr[i].sin_addr.s_addr = inet_addr(tlc_chkpt_mserv_ip[i]);
	  }
	  else{
		base_chkpt_mserv_addr[i].sin_addr.s_addr = inet_addr(MC_SERV_IP_ADDR);
	  }

	  if(connect_w(base_chkpt_mserv_client_fd[i], (struct sockaddr *) &(base_chkpt_mserv_addr[i]), sizeof(base_chkpt_mserv_addr[i])) < 0){
		printf("mserv_create_connection: Connect %d error 1 occured errno = %d\n", i, errno);
                base_chkpt_mserv_client_fd[i] = -1;
                ret = TLC_CHKPT_ABORT;
		break;
	  }
        }
        // original connection
	chkpt_mserv_client_fd[i] = socket(AF_INET, SOCK_STREAM, 0); 

	chkpt_mserv_addr[i].sin_family = AF_INET;
	
	if(tlc_chkpt_mserv_port[i] != 0){
//printf(" TLC chkpt base port[%d] = %d\n", i, tlc_chkpt_mserv_port[i]); 
		chkpt_mserv_addr[i].sin_port = htons(tlc_chkpt_mserv_port[i]);
	}
	else{
		chkpt_mserv_addr[i].sin_port = htons(MC_SERV_PORT);
	}
	
	if(tlc_chkpt_mserv_ip[i] != NULL){
//printf(" TLC mserv ip[%d] = %s\n", i, tlc_chkpt_mserv_ip[i]); 
		chkpt_mserv_addr[i].sin_addr.s_addr = inet_addr(tlc_chkpt_mserv_ip[i]);
	}
	else{
		chkpt_mserv_addr[i].sin_addr.s_addr = inet_addr(MC_SERV_IP_ADDR);
	}

	if(connect_w(chkpt_mserv_client_fd[i], (struct sockaddr *) &(chkpt_mserv_addr[i]), sizeof(chkpt_mserv_addr[i])) < 0){
		printf("mserv_create_connection: Connect %d error 1 occured errno = %d\n", i, errno);
                chkpt_mserv_client_fd[i] = -1;
                ret = TLC_CHKPT_ABORT;
		break;
	}

        memset(mserv_cmd, 0, CHKPT_CMD_LEN);

        if(
          (tlc_chkpt_type_flag == TLC_MEM_ADDR_BLOCK) ||
          (tlc_chkpt_type_flag == TLC_MEM_ADDR_BLOCK_DOUBLE_CHANNELS)
          ){
            strncpy(mserv_cmd, "PUT ", CHKPT_CMD_LEN);
            mserv_init_command_block(
                chkpt_mserv_client_fd[i],
                mserv_cmd, 
                mserv_allocate_pages[i], 
                mserv_base_page_id[i], 
                tlc_chkpt_name_str
                );

        }
        else if(
               (tlc_chkpt_type_flag == TLC_MEM_ADDR_CYCLIC) ||
               (tlc_chkpt_type_flag == TLC_MEM_ADDR_CYCLIC_DOUBLE_CHANNELS)
               ){
            strncpy(mserv_cmd, "SEND", CHKPT_CMD_LEN);
            mserv_init_command_interleave(
                chkpt_mserv_client_fd[i],
                mserv_cmd, 
                mserv_allocate_pages[i], 
                i,
                tlc_chkpt_name_str
                );
        }

        if(
          (tlc_chkpt_type_flag == TLC_MEM_ADDR_BLOCK_DOUBLE_CHANNELS) ||
          (tlc_chkpt_type_flag == TLC_MEM_ADDR_CYCLIC_DOUBLE_CHANNELS)
        ){
          // Receive message request from client
          memset(ack_message, 0, CHKPT_MSG_LEN);
          n = read_full(chkpt_mserv_client_fd[i],ack_message, CHKPT_ACK_LEN);
          if (n<0) {
            printf("Error Reading ACK Message");
            exit(1);
          }
          printf("Received %s from %d connection\n",ack_message, i);
          fflush(stdout);
        }
      }

      if(ret == TLC_CHKPT_READY){ 
        mserv_target_page_bits = tlc_target_page_bits();
      }
      else mserv_target_page_bits = -1; // used (temporarily) by an assertion in mserv_set()

    }
    else{
      printf("mserv_create_conn: unknown state transfer type  = %d \n", state_transfer_type);
      ret = TLC_CHKPT_ABORT;
    }

out_mserv_create_connection:
    return ret;
}

int mserv_init_command_block(
              int client_fd,
              char *cmd, 
              uint64_t num_pages_allocated, 
              uint64_t base_page_id, 
              char *checkpoint_name){
    int ret = -1; 

    if((ret = write_full(client_fd, cmd, CHKPT_CMD_LEN)) < 0){
        printf(" mserv_init_set: Error: Unsuccessful cmd write: %s\n", cmd);
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

    //if((ret = write_full(client_fd, checkpoint_name, CHKPT_NAME_LEN)) < 0){
    //    printf(" mserv_init_set: Error: Unsuccessful chkptname write: %s\n", checkpoint_name);
    // 	return ret;
    //}

    return ret;
}

int mserv_init_command_interleave(
              int client_fd,
              char *cmd, 
              uint64_t num_pages_allocated, 
              int  servno,
              char *checkpoint_name){
    int ret = -1; 

    if((ret = write_full(client_fd, cmd, CHKPT_CMD_LEN)) < 0){
        printf(" mserv_init_set: Error: Unsuccessful cmd write: %s\n", cmd);
    	return ret;
    }

    //if((ret = write_full(client_fd, &base_page_id, sizeof(uint64_t))) < 0){
    //    printf(" mserv_init_set: Error: Unsuccessful basepageid write %" PRIx64 "\n", base_page_id);
    // 	return ret;
    //}

    if((ret = write_full(client_fd, &num_pages_allocated, sizeof(uint64_t))) < 0){
        printf(" mserv_init_set: Error: Unsuccessful numpagealloc write %" PRIx64 "\n", num_pages_allocated);
    	return ret;
    }

    if((ret = write_full(client_fd, &servno, sizeof(int))) < 0){
        printf(" mserv_init_set: Error: Unsuccessful numpagealloc write %" PRIx64 "\n", num_pages_allocated);
    	return ret;
    }
    //if((ret = write_full(client_fd, checkpoint_name, CHKPT_NAME_LEN)) < 0){
    //    printf(" mserv_init_set: Error: Unsuccessful chkptname write: %s\n", checkpoint_name);
   // 	return ret;
    //}

    return ret;
}

int mserv_get_command(
              int client_fd,
              char *cmd, 
              uint64_t num_pages_allocated, 
              int  servno,
              char *checkpoint_name){
    int ret = -1; 

    if((ret = write_full(client_fd, cmd, CHKPT_CMD_LEN)) < 0){
        printf(" mserv_get: Error: Unsuccessful cmd write: %s\n", cmd);
    	return ret;
    }

    if((ret = write_full(client_fd, &num_pages_allocated, sizeof(uint64_t))) < 0){
        printf(" mserv_get: Error: Unsuccessful numpagealloc write %" PRIx64 "\n", num_pages_allocated);
    	return ret;
    }

    if((ret = write_full(client_fd, &servno, sizeof(int))) < 0){
        printf(" mserv_get: Error: Unsuccessful numpagealloc write %" PRIx64 "\n", num_pages_allocated);
    	return ret;
    }

    //if((ret = write_full(client_fd, checkpoint_name, CHKPT_NAME_LEN)) < 0){
    //    printf(" mserv_get: Error: Unsuccessful chkptname write: %s\n", checkpoint_name);
    // 	return ret;
    //}

    return ret;
}

int mserv_get_command_interleave(
              int client_fd,
              char *cmd, 
              uint64_t num_pages_allocated, 
              int  servno,
              char *checkpoint_name){
    int ret = -1; 

    if((ret = write_full(client_fd, cmd, CHKPT_CMD_LEN)) < 0){
        printf(" mserv_get: Error: Unsuccessful cmd write: %s\n", cmd);
    	return ret;
    }

    if((ret = write_full(client_fd, &num_pages_allocated, sizeof(uint64_t))) < 0){
        printf(" mserv_get: Error: Unsuccessful numpagealloc write %" PRIx64 "\n", num_pages_allocated);
    	return ret;
    }

    if((ret = write_full(client_fd, &servno, sizeof(int))) < 0){
        printf(" mserv_get: Error: Unsuccessful numpagealloc write %" PRIx64 "\n", num_pages_allocated);
    	return ret;
    }
    //if((ret = write_full(client_fd, checkpoint_name, CHKPT_NAME_LEN)) < 0){
    //    printf(" mserv_get: Error: Unsuccessful chkptname write: %s\n", checkpoint_name);
    // 	return ret;
    //}

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
    
    if((ret = write_full(mclient_fd, mkey, mkey_size)) < 0){
        printf(" mc_set: Error: Unsuccessful key write %" PRIx64 "\n", (uint64_t) mkey);
    	return ret;
    }

    if((ret = write_full(mclient_fd, buf, buf_size)) < 0){
        printf(" mc_set: Error: Unsuccessful page write (key = %" PRIx64 ") \n", (uint64_t) mkey);
        return ret;
    }

    return ret;   
}

int mc_set2(const void *mkey, size_t mkey_size, const void *buf, size_t buf_size){
    int ret = -1;
    
    if((ret = write_full(mclient_fd2, mkey, mkey_size)) < 0){
        printf(" mc_set2: Error: Unsuccessful key write %" PRIx64 "\n", (uint64_t) mkey);
    	return ret;
    }

    if((ret = write_full(mclient_fd2, buf, buf_size)) < 0){
        printf(" mc_set2: Error: Unsuccessful page write (key = %" PRIx64 ") \n", (uint64_t) mkey);
        return ret;
    }

    return ret;   
}

#define TLC_EOF_MARKER	0x0FF

void mc_close(void){
    uint64_t eof_flag = TLC_EOF_MARKER;
    
    if(write_full(mclient_fd, &eof_flag, sizeof(uint64_t)) < 0){
        printf(" mc_close: Error: Unsuccessful EOF key write %" PRIx64 "\n", (uint64_t) eof_flag);
    }
    close(mclient_fd);
}

void mc_close2(void){
    uint64_t eof_flag = TLC_EOF_MARKER;
    
    if(write_full(mclient_fd2, &eof_flag, sizeof(uint64_t)) < 0){
        printf(" mc_close2: Error: Unsuccessful EOF key write %" PRIx64 "\n", (uint64_t) eof_flag);
    }
    close(mclient_fd2);
}

uint64_t mserv_set_item_cnt = 0; // debugging
uint64_t base_mserv_set_item_cnt = 0; // debugging

extern int tlc_chkpt_status;
extern uint64_t per_mserv_allocate_pages;

int mserv_set(const void *mkey, size_t mkey_size, const void *buf, size_t buf_size);

int mserv_set(const void *mkey, size_t mkey_size, const void *buf, size_t buf_size){
    int ret = -1;
    
    uint64_t addr = *((uint64_t *)mkey);
    uint64_t page_id;
    // determine which server to send to based on block distribution
    uint64_t mserv_id; 
    int fd = -1;

    // assertion
    if(!((tlc_chkpt_status == TLC_CHKPT_READY)&&(mserv_target_page_bits > 0))){ 
        printf("mserv_set: assertion! error mserv_target_page_bits = % " PRId64 "\n",
          mserv_target_page_bits);
        exit(1);
    }

    // choose a memory server
    page_id = addr >> mserv_target_page_bits;
    
    if(
       (tlc_chkpt_type_flag == TLC_MEM_ADDR_BLOCK) ||
       (tlc_chkpt_type_flag == TLC_MEM_ADDR_BLOCK_DOUBLE_CHANNELS)
      ){
        mserv_id = page_id/per_mserv_allocate_pages; // for block distribution 
    }
    else if(
       (tlc_chkpt_type_flag == TLC_MEM_ADDR_CYCLIC) ||
       (tlc_chkpt_type_flag == TLC_MEM_ADDR_CYCLIC_DOUBLE_CHANNELS)
    ){
        mserv_id = page_id % chkpt_mserv_cnt;  // for cyclic distribution
    }

    if (mserv_id < chkpt_mserv_cnt){ 
        fd = chkpt_mserv_client_fd[mserv_id];
    }
    else{
        printf(" mserv_set: Abort! fatal error sending ptr = %" PRIx64 " key (addr) = %" PRId64 " page id = %" PRId64 "\n", (uint64_t)mkey, *((uint64_t *)mkey), page_id);
        printf(" mserv_set: mserv_id = %" PRId64 " chkpt_cnt = %d per_mserv = %" PRId64 " sent_item %" PRId64  "\n", mserv_id, chkpt_mserv_cnt, per_mserv_allocate_pages, mserv_set_item_cnt);
        fflush(stdout);
        exit(1);
    }
 
    // sending addr and page's contents
    mserv_set_item_cnt++;

    if((ret = write_full(fd, mkey, mkey_size)) < 0){
        printf(" mserv_set: Error: Unsuccessful key write %" PRIx64 "\n", (uint64_t) mkey);
    	return ret;
    }
    if((ret = write_full(fd, buf, buf_size)) < 0){
        printf(" mserv_set: Error: Unsuccessful value write (key = %" PRIx64 ") \n", (uint64_t) mkey);
        return ret;
    }
    return ret;   
}

// define mserv_base_set...
int mserv_base_set(const void *mkey, size_t mkey_size, const void *buf, size_t buf_size);

int mserv_base_set(const void *mkey, size_t mkey_size, const void *buf, size_t buf_size){
    int ret = -1;
    
    uint64_t addr = *((uint64_t *)mkey);
    uint64_t page_id;
    // determine which server to send to based on block distribution
    uint64_t mserv_id; 
    int fd = -1;

    // assertion
    if(!((tlc_chkpt_status == TLC_CHKPT_READY)&&(mserv_target_page_bits > 0))){ 
        printf("mserv_set: assertion! error mserv_target_page_bits = % " PRId64 "\n",
          mserv_target_page_bits);
        exit(1);
    }

    // choose a memory server
    page_id = addr >> mserv_target_page_bits;
    
    if(
       (tlc_chkpt_type_flag == TLC_MEM_ADDR_BLOCK) ||
       (tlc_chkpt_type_flag == TLC_MEM_ADDR_BLOCK_DOUBLE_CHANNELS)
      ){
        mserv_id = page_id/per_mserv_allocate_pages; // for block distribution 
    }
    else if(
       (tlc_chkpt_type_flag == TLC_MEM_ADDR_CYCLIC) ||
       (tlc_chkpt_type_flag == TLC_MEM_ADDR_CYCLIC_DOUBLE_CHANNELS)
    ){
        mserv_id = page_id % chkpt_mserv_cnt;  // for cyclic distribution
    }

    if (mserv_id < chkpt_mserv_cnt){ 
        fd = base_chkpt_mserv_client_fd[mserv_id];
    }
    else{
        printf(" mserv_set: Abort! fatal error sending ptr = %" PRIx64 " key (addr) = %" PRId64 " page id = %" PRId64 "\n", (uint64_t)mkey, *((uint64_t *)mkey), page_id);
        printf(" mserv_set: mserv_id = %" PRId64 " chkpt_cnt = %d per_mserv = %" PRId64 " sent_item %" PRId64  "\n", mserv_id, chkpt_mserv_cnt, per_mserv_allocate_pages, base_mserv_set_item_cnt);
        fflush(stdout);
        exit(1);
    }
 
    // sending addr and page's contents
    base_mserv_set_item_cnt++;

    if((ret = write_full(fd, mkey, mkey_size)) < 0){
        printf(" mserv_set: Error: Unsuccessful key write %" PRIx64 "\n", (uint64_t) mkey);
    	return ret;
    }
    if((ret = write_full(fd, buf, buf_size)) < 0){
        printf(" mserv_set: Error: Unsuccessful value write (key = %" PRIx64 ") \n", (uint64_t) mkey);
        return ret;
    }
    return ret;   
}

void mserv_close(void);

void mserv_close(void){
    int i;
    uint64_t eof_flag = TLC_EOF_MARKER;
    
    for(i = 0; i < chkpt_mserv_cnt; i++){
        if(write_full(chkpt_mserv_client_fd[i], &eof_flag, sizeof(uint64_t)) < 0){
          printf(" mserv_close: Error: Unsuccessful EOF key write %" PRIx64 "\n", (uint64_t) eof_flag);
        }
        //if(read_full(chkpt_mserv_client_fd[i], &ret_eof_flag, sizeof(uint64_t)) < 0){
        //  printf(" mserv_close: Unsuccessful EOF key read back %" PRIx64 "\n", (uint64_t) ret_eof_flag);
        //}
        //close(chkpt_mserv_client_fd[i]);
    }
}
// define mserv_base_close()
void mserv_base_close(void);

void mserv_base_close(void){
    int i;
    uint64_t eof_flag = TLC_EOF_MARKER;
    
    for(i = 0; i < chkpt_mserv_cnt; i++){
        if(write_full(base_chkpt_mserv_client_fd[i], &eof_flag, sizeof(uint64_t)) < 0){
          printf(" mserv_base_close: Error: Unsuccessful EOF key write %" PRIx64 "\n", (uint64_t) eof_flag);
        }
        //if(read_full(chkpt_mserv_client_fd[i], &ret_eof_flag, sizeof(uint64_t)) < 0){
        //  printf(" mserv_close: Unsuccessful EOF key read back %" PRIx64 "\n", (uint64_t) ret_eof_flag);
        //}
        //close(chkpt_mserv_client_fd[i]);
    }
}

// TLC server-side functions
extern void tlc_parallel_receiving_pages_priority_mutex(int fd);
extern void tlc_parallel_receiving_pages_mutexfree(int fd);
extern void tlc_barrier_synchronization_with_io_thread(void);

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

	if((mserv_conn_fd = accept_cr(mserv_lis_fd, NULL, NULL)) < 0){
		printf("Accept: Error occured\n");
		exit(1);
	}

	if((mserv_conn_fd2 = accept_cr(mserv_lis_fd2, NULL, NULL)) < 0){
		printf("Accept: Error occured\n");
		exit(1);
	}

	tlc_parallel_receiving_pages_priority_mutex(mserv_conn_fd);
	
	close(mserv_conn_fd);
	close(mserv_lis_fd);

	tlc_parallel_receiving_pages_mutexfree(mserv_conn_fd2);
	
	close(mserv_conn_fd2);
	close(mserv_lis_fd2);

        tlc_barrier_synchronization_with_io_thread();

    }
    else if(
      (state_transfer_type == TLM_EXEC)){
       // noop;
    }else if(
      (state_transfer_type == TLC_EXEC)||
      (state_transfer_type == TLC_TCP_STANDBY_DST)
    ){
        printf("tlc_incoming_t: invalid TLC state transfer type  = %d \n", state_transfer_type);
        //t = NULL;
    }
    else{
        printf("tlc_incoming_t: unknown state transfer type  = %d \n", state_transfer_type);
        //t = NULL;
    }


    pthread_exit((void *) t);	
}

// Global vars and the function below process recovery instruction string given by
// users as loading parameters when restoring a vm. The chkpt_recovery_inst_str[] 
// were produced by tlc_config() when the loading vm start incoming migration.
//
char *tlc_recovery_chkpt_name[MAX_TLC_MSERV];
char *tlc_recovery_mserv_ip[MAX_TLC_MSERV];
uint32_t tlc_recovery_mserv_port[MAX_TLC_MSERV];
uint64_t tlc_recovery_base_page_id[MAX_TLC_MSERV];
uint64_t tlc_recovery_allocate_pages[MAX_TLC_MSERV];

void tlc_processing_recovery_inst_str(int i);

void tlc_processing_recovery_inst_str(int i){
    char *tmp_ip_str, *tmp_port_str; 
    char *tmp_base_page_id_str, *tmp_allocate_pages_str;
    char *tmp_checkpoint_name_str;

    char *tmp_recovery_str = calloc(INST_STR_LEN, 1);

    memcpy(tmp_recovery_str, chkpt_recovery_inst_str[i] ,strlen(chkpt_recovery_inst_str[i])); 
    // split tlc_chkpt_mserv_ipport_str
    if((tmp_ip_str = strtok(tmp_recovery_str, ":")) == NULL){
        printf("recovery: scanning chkpt IP error on %d\n", i);
        fflush(stdout);
        goto out_processing_recovery_str;
    }
    if((tmp_port_str = strtok(NULL, ":")) == NULL){
        printf("recovery: scanning chkpt port error on %d\n", i);
        fflush(stdout);
        goto out_processing_recovery_str;
    }
    if((tmp_base_page_id_str = strtok(NULL, ":")) == NULL){
        printf("recovery: scanning base page id str error on %d\n", i);
        fflush(stdout);
        goto out_processing_recovery_str;
    }
    if((tmp_allocate_pages_str = strtok(NULL, ":")) == NULL){
        printf("recovery: scanning num alloc page str error on %d\n", i);
        fflush(stdout);
        goto out_processing_recovery_str;
    }
    if((tmp_checkpoint_name_str = strtok(NULL, ":")) == NULL){
        printf("recovery: scanning chkpt name str error on %d\n", i);
        fflush(stdout);
        goto out_processing_recovery_str;
    }

    if(tlc_recovery_mserv_ip[i] == NULL) 
        tlc_recovery_mserv_ip[i] = malloc(TLC_STR_LEN);
    memset(tlc_recovery_mserv_ip[i], 0, TLC_STR_LEN);
    memcpy(tlc_recovery_mserv_ip[i], tmp_ip_str, strlen(tmp_ip_str));

    tlc_recovery_mserv_port[i] = atoi(tmp_port_str);
    tlc_recovery_base_page_id[i] = atoi(tmp_base_page_id_str);
    tlc_recovery_allocate_pages[i] = atoi(tmp_allocate_pages_str);

    if(tlc_recovery_chkpt_name[i] == NULL) 
        tlc_recovery_chkpt_name[i] = malloc(TLC_STR_LEN);
    memset(tlc_recovery_chkpt_name[i], 0, TLC_STR_LEN);
    memcpy(tlc_recovery_chkpt_name[i], tmp_checkpoint_name_str, strlen(tmp_checkpoint_name_str));

printf(" [%d] recovery_ip = %s port %d base_id = %" PRId64 " alloc_pages = % " PRId64 " chkname = %s\n", 
i, tlc_recovery_mserv_ip[i], tlc_recovery_mserv_port[i], 
tlc_recovery_base_page_id[i], tlc_recovery_allocate_pages[i], 
tlc_recovery_chkpt_name[i]);
fflush(stdout);

out_processing_recovery_str: 
    free(tmp_recovery_str);
    return; 
}

char *tlc_origparam_chkpt_name[MAX_TLC_MSERV];
char *tlc_origparam_mserv_ip[MAX_TLC_MSERV];
uint32_t tlc_origparam_mserv_port[MAX_TLC_MSERV];
uint64_t tlc_origparam_base_page_id[MAX_TLC_MSERV];
uint64_t tlc_origparam_allocate_pages[MAX_TLC_MSERV];

void tlc_processing_origparam_inst_str(int i);

void tlc_processing_origparam_inst_str(int i){
    char *tmp_ip_str, *tmp_port_str; 
    char *tmp_base_page_id_str, *tmp_allocate_pages_str;
    char *tmp_checkpoint_name_str;

    char *tmp_recovery_str = calloc(INST_STR_LEN, 1);

    memcpy(tmp_recovery_str, chkpt_recovery_inst_str[i] ,strlen(chkpt_recovery_inst_str[i])); 
    // split tlc_chkpt_mserv_ipport_str
    if((tmp_ip_str = strtok(tmp_recovery_str, ":")) == NULL){
        printf("origparam: scanning chkpt IP error on %d\n", i);
        fflush(stdout);
        goto out_processing_recovery_str;
    }
    if((tmp_port_str = strtok(NULL, ":")) == NULL){
        printf("orig recovery: scanning chkpt port error on %d\n", i);
        fflush(stdout);
        goto out_processing_recovery_str;
    }
    if((tmp_base_page_id_str = strtok(NULL, ":")) == NULL){
        printf("orig recovery: scanning base page id str error on %d\n", i);
        fflush(stdout);
        goto out_processing_recovery_str;
    }
    if((tmp_allocate_pages_str = strtok(NULL, ":")) == NULL){
        printf("orig recovery: scanning num alloc page str error on %d\n", i);
        fflush(stdout);
        goto out_processing_recovery_str;
    }
    if((tmp_checkpoint_name_str = strtok(NULL, ":")) == NULL){
        printf("orig recovery: scanning chkpt name str error on %d\n", i);
        fflush(stdout);
        goto out_processing_recovery_str;
    }

    if(tlc_origparam_mserv_ip[i] == NULL) 
        tlc_origparam_mserv_ip[i] = malloc(TLC_STR_LEN);
    memset(tlc_origparam_mserv_ip[i], 0, TLC_STR_LEN);
    memcpy(tlc_origparam_mserv_ip[i], tmp_ip_str, strlen(tmp_ip_str));

    tlc_origparam_mserv_port[i] = atoi(tmp_port_str);
    tlc_origparam_base_page_id[i] = atoi(tmp_base_page_id_str);
    tlc_origparam_allocate_pages[i] = atoi(tmp_allocate_pages_str);

    if(tlc_origparam_chkpt_name[i] == NULL) 
        tlc_origparam_chkpt_name[i] = malloc(TLC_STR_LEN);
    memset(tlc_origparam_chkpt_name[i], 0, TLC_STR_LEN);
    memcpy(tlc_origparam_chkpt_name[i], tmp_checkpoint_name_str, strlen(tmp_checkpoint_name_str));

printf(" origparam [%d] recovery_ip = %s port %d base_id = %" PRId64 " alloc_pages = % " PRId64 " chkname = %s\n", 
i, tlc_origparam_mserv_ip[i], tlc_origparam_mserv_port[i], 
tlc_origparam_base_page_id[i], tlc_origparam_allocate_pages[i], 
tlc_origparam_chkpt_name[i]);
fflush(stdout);

out_processing_recovery_str: 
    free(tmp_recovery_str);
    return; 
}

int matching_chkpt_names(int src_chkpt_serv_num);

int matching_chkpt_names(int src_chkpt_serv_num){
    int i; 
    if(src_chkpt_serv_num != chkpt_mserv_cnt){
       printf("Unequal number of recovery servers\n");
       fflush(stdout);
       return 0;
    }

    for(i = 0; i < chkpt_mserv_cnt; i++){
       if(strncmp(tlc_origparam_chkpt_name[i], tlc_recovery_chkpt_name[i], strlen(tlc_origparam_chkpt_name[i])) != 0){
           printf("Unmatch chkpt name: orig %s != param %s \n", 
               tlc_origparam_chkpt_name[i], tlc_recovery_chkpt_name[i]);
           fflush(stdout);
           return 0;
       }
    }

    return 1;

}


void tlc_recovery_ram(int i);

void tlc_recovery_ram(int i){
    char mserv_cmd[CHKPT_CMD_LEN];

    chkpt_mserv_client_fd[i] = socket(AF_INET, SOCK_STREAM, 0); 
    chkpt_mserv_addr[i].sin_family = AF_INET;
	
    if(tlc_recovery_mserv_port[i] != 0){
printf(" TLC recovery base port[%d] = %d\n", i, tlc_recovery_mserv_port[i]); 
fflush(stdout);
	chkpt_mserv_addr[i].sin_port = htons(tlc_recovery_mserv_port[i]);
    }
    else{
	chkpt_mserv_addr[i].sin_port = htons(MC_SERV_PORT);
    }
	
    if(tlc_recovery_mserv_ip[i] != NULL){
printf(" TLC recovery mserv ip[%d] = %s\n", i, tlc_recovery_mserv_ip[i]); 
fflush(stdout);
	chkpt_mserv_addr[i].sin_addr.s_addr = inet_addr(tlc_recovery_mserv_ip[i]);
    }
    else{
	chkpt_mserv_addr[i].sin_addr.s_addr = inet_addr(MC_SERV_IP_ADDR);
    }

printf(" TLC recovery making [%d] connection \n", i); 
fflush(stdout);
    if(connect_w(chkpt_mserv_client_fd[i], (struct sockaddr *) &(chkpt_mserv_addr[i]), sizeof(chkpt_mserv_addr[i])) < 0){
	printf("tlc_recovery_ram: Connect %d error occured errno = %d\n", i, errno);
        exit(1);
    }

    if(
        (tlc_chkpt_type_flag == TLC_MEM_ADDR_BLOCK) ||
        (tlc_chkpt_type_flag == TLC_MEM_ADDR_BLOCK_DOUBLE_CHANNELS)
      ){
        memset(mserv_cmd, 0, CHKPT_CMD_LEN);
        strncpy(mserv_cmd, "RECV", CHKPT_CMD_LEN);

printf(" TLC recovery sending mserv_get [%d]\n", i); 
fflush(stdout);
        mserv_get_command(
              chkpt_mserv_client_fd[i],
              mserv_cmd, 
              tlc_recovery_allocate_pages[i], 
              i,
              tlc_recovery_chkpt_name[i]
              );

        tlc_parallel_receiving_pages_mutexfree(chkpt_mserv_client_fd[i]); 
        close(chkpt_mserv_client_fd[i]);
    }
    else if(
            (tlc_chkpt_type_flag == TLC_MEM_ADDR_CYCLIC) ||
            (tlc_chkpt_type_flag == TLC_MEM_ADDR_CYCLIC_DOUBLE_CHANNELS)
        ){
        memset(mserv_cmd, 0, CHKPT_CMD_LEN);
        strncpy(mserv_cmd, "RECV", CHKPT_CMD_LEN);

printf(" TLC recovery sending mserv_get [%d]\n", i); 
fflush(stdout);
        mserv_get_command_interleave(
              chkpt_mserv_client_fd[i],
              mserv_cmd, 
              tlc_recovery_allocate_pages[i], 
              i,
              tlc_recovery_chkpt_name[i]
              );

        tlc_parallel_receiving_pages_mutexfree(chkpt_mserv_client_fd[i]); 
        close(chkpt_mserv_client_fd[i]);
    }
     
}

void *tlc_recovery_t(void *t);

void *tlc_recovery_t(void *t){

    long i = (long)t;
    int64_t thr_restore_start, thr_restore_end; 

    thr_restore_start = qemu_get_clock_ms(rt_clock);

    tlc_recovery_ram(i);

    // timing                    
    thr_restore_end = qemu_get_clock_ms(rt_clock);
    printf("tlc_recovery_ram(%ld): Elasped time = %" PRId64 " ms\n", i,
	    thr_restore_end - thr_restore_start);

    pthread_exit((void *)t);
}

// -----Basic Communication Utilities-----
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

int 	vic_fd = 0; 
struct 	sockaddr_in vic_addr;

int vic_create_connection(int num_conn);

int vic_create_connection(int num_conn){
        char vic_message[150];
	int ret;

        if(vic_fd > 0){
            printf(" vic fd already exist = %d\n", vic_fd); 
            fflush(stdout);
            return -1;
        }

	vic_fd = socket(AF_INET, SOCK_STREAM, 0); 

	vic_addr.sin_family = AF_INET;
	
	if(vic_port != 0){
//printf(" VIC conn vic port = %d\n", vic_port); 
		vic_addr.sin_port = htons(vic_port);
	}
	else{
		vic_addr.sin_port = htons(MC_VIC_PORT);
	}
	
	if(vic_ip != NULL){
//printf(" VIC conn vic ip = %s\n", vic_ip); 
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

