/*  This program is a part of TLC memory server system.
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

#include "tlc-memory-server.h"

#include <time.h>
#include <sys/time.h>

struct timeval startsave;
struct timeval endsave;
struct timeval endmemsave;
struct timeval endfilesave;
uint64_t totalsave_elasp; 
uint64_t memsave_elasp; 
uint64_t filesave_elasp;

struct timeval startret;
struct timeval endret;
struct timeval endmemret;
struct timeval endfileret;
uint64_t totalret_elasp; 
uint64_t memret_elasp; 
uint64_t fileret_elasp;

// Socket utilities
int connect_w(int fd, const struct sockaddr *addr, socklen_t len);
int accept_cr(int fd, struct sockaddr *addr, socklen_t *len);
int select_w(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
int write_full(int fd, const void *buf, size_t count);
int read_full(int fd, void *buf, size_t count);

// Receiving/Sending pages
int recv_init_command(int client_fd);
int recv_get_command(int client_fd);
void tlc_receiving_pages(int fd);
void tlc_receiving_pages_no_ack(int fd);
void tlc_sending_pages(int fd);

// VIC Stuffs
int vic_create_connection(int num_conn);
int vic_report(char *message);

extern int 	mem_service_init(int memory_size);
extern uint8_t 	*get_global_mem_ptr(ram_addr_t addr);
extern uint8_t 	*add_ram_page(ram_addr_t addr);
extern uint64_t get_global_page_size(void);
extern void 	mem_service_finish(void);
extern void 	reset_global_mem_ptr(void);
extern uint8_t	*get_next_global_mem_ptr(ram_addr_t *current_addr);

typedef uint64_t ram_addr_t;

// Basic data structures for a memory server
int 	tlc_mserv_base_port = MC_SERV_PORT;
char 	cmdstr[CHKPT_CMD_LEN];
uint64_t base_page_id; 
uint64_t base_addr; 
uint64_t num_pages_allocated; 
char 	current_checkpoint_name[CHKPT_NAME_LEN];
char 	retrieve_checkpoint_name[CHKPT_NAME_LEN];
char 	*chkpt_recovery_inst = NULL;

int	vic_flag = 0;
int 	vic_port;
char 	*vic_ip;

int 	vic_fd; 
struct 	sockaddr_in vic_addr;

// Socket utility implementation
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

// TLC receiving pages from a checkpointed VM
void tlc_receiving_pages(int fd){
    ram_addr_t addr;
    int      tlc_eof, ret, local_page_size; 
    int      received_pages = 0;
    size_t   key_length = sizeof(uint64_t); 
    size_t   value_length = sizeof(uint8_t)*TARGET_PAGE_SIZE;
    uint8_t  *p;

    while(1){
        // receiving addr
        if((tlc_eof = read_full(fd, &addr, key_length)) < 0){
	    printf("mem server: network read error\n");
            fflush(stdout);
            goto out_receiving_pages;
	}

	if(addr == TLC_EOF_MARKER){  
	    printf("mem server: TLC_EOF_MARKER caught\n");
            if((ret = write_full(fd, &addr, key_length)) < 0){
               printf(" mem_server: error: Unsuccessful send eof back ret = %d\n", ret);
               fflush(stdout);
               goto out_receiving_pages;
            }
	    break; // This is the right way out of this loop
	}

	if(tlc_eof == 0){
	    printf("mem server: EOF caught\n");
            fflush(stdout);
            goto out_receiving_pages;
	}
		
	addr &= TARGET_PAGE_MASK;
	if((p = get_global_mem_ptr(addr)) == NULL){
            p = add_ram_page(addr);
        }

        if(p == NULL){
	    printf("mem server: s'th horrible happen! p is NULL, Abort!\n");
            fflush(stdout);
	    exit(1);
        }

        // receiving page contents
	tlc_eof = read_full(fd, p, value_length);

	if(tlc_eof == 0){
	    printf("mem server: error eof received \n");
            fflush(stdout);
            goto out_receiving_pages;
	}
	else if (tlc_eof < 0){
	    printf("mem server: Abort error ret = %d\n", tlc_eof);
	    exit(1);
	}
	else{
	    received_pages++;		
	}
    }
    local_page_size = get_global_page_size();

    printf("mem server: recv received_pages = %d global_page_size = %d\n", 
        received_pages, local_page_size);
    fflush(stdout);

    if(vic_flag){
        char progress_string[VIC_STR_LEN];
        sprintf(progress_string,"g22 %d %d\n", received_pages, local_page_size);
	vic_report(progress_string);
    }
out_receiving_pages: 
    close(fd);
}

// TLC receiving pages from a checkpointed VM
void tlc_receiving_pages_no_ack(int fd){
    ram_addr_t addr;
    int      tlc_eof, ret, local_page_size; 
    int      received_pages = 0;
    size_t   key_length = sizeof(uint64_t); 
    size_t   value_length = sizeof(uint8_t)*TARGET_PAGE_SIZE;
    uint8_t  *p;

    while(1){
        // receiving addr
        if((tlc_eof = read_full(fd, &addr, key_length)) < 0){
	    printf("mem server: network read error\n");
            fflush(stdout);
            goto out_receiving_pages;
	}

	if(addr == TLC_EOF_MARKER){  
	    break; // This is the right way out of this loop
	}

	if(tlc_eof == 0){
	    printf("mem server: EOF caught\n");
            fflush(stdout);
            goto out_receiving_pages;
	}
		
	addr &= TARGET_PAGE_MASK;
	if((p = get_global_mem_ptr(addr)) == NULL){
            p = add_ram_page(addr);
        }

        if(p == NULL){
	    printf("mem server: s'th horrible happen! p is NULL, Abort!\n");
            fflush(stdout);
	    exit(1);
        }

        // receiving page contents
	tlc_eof = read_full(fd, p, value_length);

	if(tlc_eof == 0){
	    printf("mem server: error eof received \n");
            fflush(stdout);
            goto out_receiving_pages;
	}
	else if (tlc_eof < 0){
	    printf("mem server: Abort error ret = %d\n", tlc_eof);
	    exit(1);
	}
	else{
	    received_pages++;		
	}
    }
    local_page_size = get_global_page_size();

    printf("mem server: recv received_pages = %d local_page_size = %d\n", 
        received_pages, local_page_size);
    fflush(stdout);

    if(vic_flag){
        char progress_string[VIC_STR_LEN];
        sprintf(progress_string,"g22 %d %d\n", received_pages, local_page_size);
	vic_report(progress_string);
    }
out_receiving_pages: 
    close(fd);
}

// TLC sending pages to a recovery VM
void tlc_sending_pages(int fd){
    ram_addr_t addr;
    uint64_t page_id;
    int      tlc_ret; 
    uint8_t *p = NULL;
    size_t   key_length = sizeof(uint64_t); 
    size_t   value_length = sizeof(uint8_t)*TARGET_PAGE_SIZE;
    uint64_t sent_pages = 0;
    uint64_t eof_flag = TLC_EOF_MARKER;

    reset_global_mem_ptr();

    while(1){
        if((p = get_next_global_mem_ptr(&addr)) == NULL){
            // The returned addr is already adjusted for the dst VM  
            printf("tlc sending: no more stored pages, page sent = %d\n", sent_pages);
            fflush(stdout);
            break; // this is the right point to finish the loop
	}
        if((tlc_ret = write_full(fd, &addr, key_length)) < 0){
	    printf("tlc sending: network write error\n");
            fflush(stdout);
            break;
	}

	tlc_ret = write_full(fd, p, value_length);
	if(tlc_ret == 0){
	    printf("tlc sending: error cannot send data\n");
            fflush(stdout);
            break;
	}
	else if (tlc_ret < 0){
	    printf("tlc sending: Abort error ret %d\n", tlc_ret);
	    exit(1);
	}
	else{
	    sent_pages++;		
	}
    }
    
    if(write_full(fd, &eof_flag, sizeof(uint64_t)) < 0){
        printf(" tlc sending: error unsuccessful EOF key write %" PRIx64 "\n", (uint64_t) eof_flag);
        fflush(stdout);
    }

    close(fd);
}

// memory server's listening and connecting sockets 
int mserv_lis_fd;
int mserv_conn_fd; 
struct sockaddr_in mserv_addr;

int recv_init_command(int client_fd){
    int ret = -1; 

    if((ret = read_full(client_fd, &base_page_id, sizeof(uint64_t))) < 0){
        printf(" recv_init: Error: Unsuccessful basepageid read ret = %d\n", ret);
        fflush(stdout);
    	return ret;
    }

    base_addr = base_page_id * TARGET_PAGE_SIZE;

    if((ret = read_full(client_fd, &num_pages_allocated, sizeof(uint64_t))) < 0){
        printf(" recv_init: Error: Unsuccessful allocpages read ret = %d\n", ret);
        fflush(stdout);
    	return ret;
    }

    if((ret = read_full(client_fd, current_checkpoint_name, CHKPT_NAME_LEN)) < 0){
        printf(" recv_init: Error: Unsuccessful chkptname read ret = %d\n", ret);
        fflush(stdout);
    	return ret;
    }

    printf(" recv_init: baseid=%" PRId64 ", numpage=%" PRId64 ", chkname=%s, ret = %d\n", 
          base_page_id,
          num_pages_allocated,
          current_checkpoint_name,
          ret);
    fflush(stdout);

    if(chkpt_recovery_inst == NULL)
        chkpt_recovery_inst = malloc(INST_STR_LEN);
    memset(chkpt_recovery_inst, 0, INST_STR_LEN);

    snprintf(
        chkpt_recovery_inst, INST_STR_LEN,
        "%" PRId64 ":%" PRId64 ":%s",
              base_page_id, 
              num_pages_allocated, 
              current_checkpoint_name);
 
    printf(" recv init: chkpt name = %s\n recover instr = %s\n", current_checkpoint_name, chkpt_recovery_inst);
    fflush(stdout);

    return ret;
}

int recv_get_command(int client_fd){
    int ret = -1; 

    if((ret = read_full(client_fd, retrieve_checkpoint_name, CHKPT_NAME_LEN)) < 0){
        printf(" recv retrieve: Error: Unsuccessful chkptname read ret = %d\n", ret);
        fflush(stdout);
    	return ret;
    }

    printf(" recv retrieve: retrieve chkpt name = %s\n ", retrieve_checkpoint_name);
    fflush(stdout);

    return ret;
}


int main(int argc, char *argv[]){
    int ret; 
    char tlc_prefix[TLC_STR_LEN];

    // chkpt name follows prefix.checkpoint_name
    if(argc == 3){
        
        if(strlen(argv[1]) >= TLC_STR_LEN){
          printf("prefix too long (> %d)\n", TLC_STR_LEN);
          exit(1);
        }
        else{
          memcpy(tlc_prefix, argv[1], strlen(argv[1]));
        }
        tlc_mserv_base_port = atoi(argv[2]); 
        printf("cmd: %s %s %d\n", argv[0], argv[1], tlc_mserv_base_port);
        fflush(stdout);
    }
    else{
        printf("invalid arguments\n");
        exit(1);
    }

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

    while(1){

        printf("mem server: accepting state\n");
        fflush(stdout); 
	if((mserv_conn_fd = accept_cr(mserv_lis_fd, NULL, NULL)) < 0){
	    printf("mem server: accept Error occured\n");
            fflush(stdout); 
	    exit(1);
	}

        gettimeofday(&startsave, NULL); // Start timing 
        startret = startsave;

        if((ret = read_full(mserv_conn_fd, cmdstr, CHKPT_CMD_LEN)) < 0){
	    printf("mem server: network read error ret = %d\n", ret);
            fflush(stdout); 
	    break; 
	}

        if(strncmp(cmdstr, "PUT ", CHKPT_CMD_LEN) == 0){
	    printf("mem_server: receives PUT\n");
	    fflush(stdout);  
	    memset(current_checkpoint_name, 0, CHKPT_NAME_LEN);
            // read the rest of the command 
            recv_init_command(mserv_conn_fd);

            mem_service_init(num_pages_allocated);

	    tlc_receiving_pages(mserv_conn_fd); 

            gettimeofday(&endmemsave, NULL);
            memsave_elasp = ((endmemsave.tv_sec * 1000000 + endmemsave.tv_usec)-(startsave.tv_sec * 1000000 + startsave.tv_usec));
            printf("Elasp time saving pages from VM to memory = %"PRId64" ms\n", (uint64_t)(memsave_elasp/1000));

        }
        else if(strncmp(cmdstr, "GET ", CHKPT_CMD_LEN) == 0){
            int current_checkpoint_name_len; 
	    printf("mem_server: receives GET\n");
	    fflush(stdout); 

	    memset(retrieve_checkpoint_name, 0, CHKPT_NAME_LEN);
            if(current_checkpoint_name != NULL){
                current_checkpoint_name_len = strlen(current_checkpoint_name);
            }
            recv_get_command(mserv_conn_fd); // receive chkpt name 
            if(strncmp(current_checkpoint_name, retrieve_checkpoint_name, current_checkpoint_name_len) == 0){
                tlc_sending_pages(mserv_conn_fd);
            }

            gettimeofday(&endmemret, NULL);
            memret_elasp = ((endmemret.tv_sec * 1000000 + endmemret.tv_usec)-(startret.tv_sec * 1000000 + startret.tv_usec));
            printf("Elasp time sending pages to recovery VM = %"PRId64" ms\n", (uint64_t)(memret_elasp/1000));

        }
/*
        else if(strncmp(cmdstr, "STORE", 5) == 0){
	    printf("mem_server: receives STORE\n");
	    fflush(stdout);  
	    memset(current_checkpoint_name, 0, CHKPT_NAME_LEN);
            // read the rest of the command 
            recv_init_command(mserv_conn_fd);
            mem_service_init(num_pages_allocated);
	    tlc_receiving_pages(mserv_conn_fd); 

            if((chkpt_fd = create_chkpt_file(tlc_prefix, current_checkpoint_name) < 0)){
              printf("mem_server: skip storing data\n");
              continue;
            }
            tlc_sending_pages(chkpt_fd);

            //meta_chkpt_fd = create_chkpt_meta_file(); 
            //tlc_store_meta_file(meta_chkpt_fd);
            close(chkpt_fd);
        }
        else if(strncmp(cmdstr, "LOAD", 4) == 0){
            int current_checkpoint_name_len; 
	    printf("mem_server: receives LOAD\n");
	    fflush(stdout); 

	    memset(retrieve_checkpoint_name, 0, CHKPT_NAME_LEN);
            if(current_checkpoint_name != NULL){
                current_checkpoint_name_len = strlen(current_checkpoint_name);
            }

            recv_get_command(mserv_conn_fd); // receive chkpt name 
            if(strncmp(current_checkpoint_name, retrieve_checkpoint_name, current_checkpoint_name_len) == 0){
                tlc_sending_pages(mserv_conn_fd);
            }
            else{
                chkpt_fd = load_chkpt_file(tlc_prefix, current_checkpoint_name);
	        tlc_receiving_pages_no_ack(chkpt_fd); 
                close(chkpt_fd);

                tlc_sending_pages(mserv_conn_fd);
            }
        }
*/
        else if(strncmp(cmdstr, "CLEAR", 5) == 0){
	    printf("mem_server: receives CLEAR\n");
	    fflush(stdout); 

            mem_service_finish();
        }
        else{
            printf("mem_server: unknown command %s\n", cmdstr);
            fflush(stdout);
        }

    }

    close(mserv_lis_fd);

}


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
	    vic_flag = 0;
	}
        // register to vic server

        if(vic_flag == 1){
            sprintf(vic_message, "vmRegis %s", current_checkpoint_name);

            printf(" vic_connect: sending a message %s\n", vic_message);
            if((ret = write_full(vic_fd, vic_message, strlen(vic_message))) < 0){
                printf(" vic_connect: Error: fail sending a message %s\n", vic_message);
		vic_flag = 0;
            }
	}

	return ret;
}

int vic_report(char *message){
    int ret; 
    if((ret = write_full(vic_fd, message, strlen(message))) < 0){
        printf(" vic_report: Error: fail sending a message %s\n", message);
    }
    return ret;
}

