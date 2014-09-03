/* store.c 
   written by Rutrada Yenyuak and Kasidit Chanchio
   copyright vasabilab 2012
*/
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <inttypes.h>
#include <stdint.h>
#include <pthread.h>

#include <time.h>
#include <sys/time.h>

#include <sys/stat.h>
#include <fcntl.h>

struct timeval startsave;
struct timeval endsave;
struct timeval endmemsave;
struct timeval endfilesave;
uint64_t totalsave_elasp; 
uint64_t memsave_elasp; 
uint64_t filesave_elasp;

struct timeval startret;
struct timeval endret;
struct timeval startmemret;
struct timeval endmemret;
struct timeval startfileret;
struct timeval endfileret;
uint64_t totalret_elasp; 
uint64_t memret_elasp; 
uint64_t fileret_elasp;

#define TARGET_PAGE_BITS 12
#define TARGET_PAGE_SIZE ((uint64_t) (1 << TARGET_PAGE_BITS))
#define TARGET_PAGE_MASK ~(TARGET_PAGE_SIZE - 1)

#define REQ_CMD     "RECV"
#define REQ_CMD_LEN 4 

#define MY_EOF      	 0x0FF

#define CHKPT_NAME_LEN	128

typedef struct {
  uint8_t memnode[TARGET_PAGE_SIZE];
} mempage;

mempage **values = NULL;
uint64_t total_pages,total_pages_param;
uint64_t base_page_id;
int servno,num_serv, port_num;

typedef uint64_t ram_addr_t;
int connected;
char *chkpt_filename = NULL;

long t1 = 1;
int read_file_done = 0;
pthread_t thread;
pthread_attr_t attr;
pthread_mutex_t mutex;
pthread_cond_t read_file_cond;

void *read_chk_file(void);
int memserver_send_eof(void);
int accept_cr(int fd, struct sockaddr *addr, socklen_t *len);
int write_full(int fd, const void *buf, size_t count);
int read_full(int fd, void *buf, size_t count);

int main(int argc, char *argv[])
{

  int sock,bytes_recieved,flagtrue = 1 ;
  ram_addr_t  adr,k,index=0,flags,pageid;
  int j=0,n,count=0,count_send=0;
  int client_fd,server_fd,client_size;
  struct sockaddr_in server_addr,client_addr;
  int sin_size,bytes_send=0;
  char message[5];

  if(argc == 6){
      num_serv = atoi(argv[1]);
      port_num = atoi(argv[2]);
      total_pages_param = atoi(argv[3]);
      base_page_id = atoi(argv[4]);
      chkpt_filename = argv[5];

      printf(" restoring num_serv = %d port_num = %d total_pages_param = %" PRId64 " base_page_id = %"  PRId64 "  chkpt_filename = %s\n", num_serv, port_num, total_pages_param, base_page_id, chkpt_filename);
      fflush(stdout);
  }
  else{
      printf(" invalid parameter! command must be in the format of \n ./restore-interleave num_serv port_num total_pages base_page_id chkpt_filename\n");
      exit(1);
  }


  /* Initialize mutex and condition variable objects */
  pthread_mutex_init(&mutex, NULL);
  pthread_cond_init (&read_file_cond, NULL);

  /* For portability, explicitly create threads in a joinable state */
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_create(&thread, &attr, (void *) &read_chk_file, (void *) NULL);

  // Create a socket
  if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
      perror("Socket");
      exit(1);
  }
  if (setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,&flagtrue,sizeof(int)) == -1) {
      perror("Setsockopt");
      exit(1);
  }
  // Identify IP address and Port
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(port_num);
  server_addr.sin_addr.s_addr = INADDR_ANY;
  bzero(&(server_addr.sin_zero),8);
  // Bind an IP address to the socket
  if (bind(sock, (struct sockaddr *)&server_addr, sizeof(struct sockaddr))== -1) {
      perror("Unable to bind");
      exit(1);
  }
  // Specify a size of a request queue
  if (listen(sock, 5) == -1) {
      perror("Listen");
      exit(1);
  }
  printf("\nRecovery BLOCK MEM Server Waiting for client\n");
  fflush(stdout);

  // Main Server Loop - accept and handle request
  while(1){
      sin_size = sizeof(client_addr);
      connected = accept_cr(sock, (struct sockaddr *)&client_addr,&sin_size);

      if (connected < 0) {
	  printf("Error Accept\n");
	  exit(1);
      }      

      printf("\n I got a connection from (%s , %d)\n",
                   inet_ntoa(client_addr.sin_addr),ntohs(client_addr.sin_port));
      fflush(stdout);

      gettimeofday(&startsave, NULL); // Start timing 
      startret = startsave;

      // Receive message request from client
      n=read_full(connected,message, REQ_CMD_LEN); // SEND/RECV is expected
      if (n<0) {
          printf("Error Reading Request Message");
          exit(1);
      }
      printf("Received Message Request is %s ,  %d \n",message,n);
      fflush(stdout);

      // Received Total Pages No.
      n=read_full(connected,&total_pages,sizeof(total_pages));
      if (n<0) {
          printf("Error Reading Total Pages");
          exit(1);
      }
      printf("Received Total Pages= %" PRId64 " and Total Pages Param= %" PRId64 " \n",total_pages, total_pages_param);
      fflush(stdout);

      // Received Total Pages No.
      n=read_full(connected,&servno,sizeof(servno));
      if (n<0) {
          printf("Error Reading serv no");
          exit(1);
      }

      // Handle requesting command
      if(strcmp(message,REQ_CMD)==0){

          // sync with read_chk_file()
          pthread_mutex_lock(&mutex);
          while (!read_file_done){ 
            pthread_cond_wait(&read_file_cond , &mutex);
          }
          pthread_mutex_unlock(&mutex);

          gettimeofday(&startmemret, NULL);

          for(k=0;k<total_pages;k++){
              //1.send addr
	      //2.send values
              if(*(values+k)!=NULL){
                  pageid = k+base_page_id;//change variable
                  adr = pageid*TARGET_PAGE_SIZE;
//		  printf("k=%d ,adr=%"PRIx64"\n",k,adr);
                  bytes_send = write_full(connected,&adr,sizeof(adr));
                  if (bytes_send == -1) {
                      fprintf(stderr,"send adr failed :%s\n",strerror(errno));
                      exit(1);
                  }//end if
                  bytes_send = write_full(connected,values[k],sizeof(mempage));
                  if (bytes_send == -1) {
        	      fprintf(stderr,"send values[k] failed :%s\n",strerror(errno));
                      exit(1);
                  }//end if
                  count_send++;
//                printf("n=%d k=%"PRIx64"  ,adr=%"PRIx64"\n",count_send,k,adr);
//                fflush(stdout);
              } //end if NULL
          }//end for k
          memserver_send_eof();

	  printf("file count_send = %d\n",count_send);
	  fflush(stdout);

          close(connected);

          gettimeofday(&endmemret, NULL);
          memret_elasp = ((endmemret.tv_sec * 1000000 + endmemret.tv_usec)-(startmemret.tv_sec * 1000000 + startmemret.tv_usec));
          printf("Elasp time sending mem pages to recovery VM = %"PRId64" ms\n", (uint64_t)(memret_elasp/1000));
 
      }    

  } //end while(1) 
  return 0;
}


void *read_chk_file(void){

    int bytes_received = 0;
    int file_fd; 
    ram_addr_t  adr,index=0,flags;
    int j=0,n,count=0,count_allocate =0;

    gettimeofday(&startfileret, NULL);

    if((file_fd = open(chkpt_filename, O_RDONLY))<0){
        printf("mem server: cannot open chk file for read error no = %d\n", errno);
        fflush(stdout);
        exit(1);
    }

    if(values == NULL){
        values = (mempage**) calloc (total_pages_param,sizeof(mempage *));
    }
    // read contents
    bytes_received =read_full(file_fd,&adr,sizeof(adr));
             
    if (bytes_received == -1) {
        fprintf(stderr,"recv addr failed :%s\n",strerror(errno));
        exit(1);
    }

    flags = adr &~TARGET_PAGE_MASK ;
    adr &= TARGET_PAGE_MASK ;
	
    while(flags != MY_EOF){
        count++;
        index = adr >> TARGET_PAGE_BITS ;
        index = index/num_serv ;

//      2.Recv data-page
        if(*(values+index)==NULL){
          values[index]=(mempage*)malloc(sizeof(mempage));
          count_allocate++;
	}
	bytes_received =read_full(file_fd,values[index],sizeof(mempage));
                     
	if (bytes_received == -1) {
	  fprintf(stderr,"recv values failed :%s\n",strerror(errno));
	  exit(1);
	}
        bytes_received =read_full(file_fd,&adr,sizeof(adr));
	if (bytes_received == -1) {
          fprintf(stderr,"recv addr failed :%s\n",strerror(errno));
          exit(1);
	}	

     	flags = adr &~TARGET_PAGE_MASK ;
	adr &= TARGET_PAGE_MASK ;

    }//end while MY_EOF
    close(file_fd);

    printf("read file count recv = %d ,count_allocate = %d \n",count,count_allocate);
    fflush(stdout);
              
    gettimeofday(&endfileret, NULL);
    fileret_elasp = ((endfileret.tv_sec * 1000000 + endfileret.tv_usec)-(startfileret.tv_sec * 1000000 + startfileret.tv_usec));
    printf("Elasp time retrieving pages from file = %"PRId64" ms\n", (uint64_t)(fileret_elasp/1000));

    pthread_mutex_lock(&mutex);
    read_file_done = 1;
    pthread_cond_signal(&read_file_cond);
    pthread_mutex_unlock(&mutex);

    pthread_exit(NULL);
}

int memserver_send_eof(void){
  // Sending addr+flag and *ptr
  int ret;
  uint8_t *pp;
  ram_addr_t          addr = 0;

  addr= (addr|MY_EOF);
  ret = write_full(connected,&addr,sizeof(addr));
  return ret;
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
            break;
        }

        count -= ret;
        buf += ret;
        total += ret;
    }

    return total;
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
            break;
        }

        count -= ret;
        buf += ret;
        total += ret;
    }

    return total;
}
