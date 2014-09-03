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
struct timeval endmemret;
struct timeval endfileret;
uint64_t totalret_elasp; 
uint64_t memret_elasp; 
uint64_t fileret_elasp;

#define TARGET_PAGE_BITS 12
#define TARGET_PAGE_SIZE ((uint64_t) (1 << TARGET_PAGE_BITS))
#define TARGET_PAGE_MASK ~(TARGET_PAGE_SIZE - 1)

#define REQ_CMD     "SEND"
#define REQ_CMD_LEN 4 

#define MY_EOF      	 0x0FF

#define CHKPT_NAME_LEN	128

typedef struct {
  uint8_t memnode[TARGET_PAGE_SIZE];
} mempage;

uint64_t total_pages,total_pages_bck;
typedef uint64_t ram_addr_t;
int connected;
char *chkpt_filename = NULL;
char *vm_name = NULL;

void create_restoration_script(uint64_t tp);
int memserver_send_eof(void);
int accept_cr(int fd, struct sockaddr *addr, socklen_t *len);
int write_full(int fd, const void *buf, size_t count);
int read_full(int fd, void *buf, size_t count);

int main(int argc, char *argv[])
{
  int j=0,n,count=0,count_send=0,count_allocate =0;
  int client_fd,server_fd,client_size;
  int sock,bytes_recieved,true = 1 ;
  struct sockaddr_in server_addr,client_addr;
  int sin_size,bytes_send=0;
  int servno,num_serv, port_num;
  char message[5];
  ram_addr_t  adr,k,index=0,flags,pageid;
  mempage **values = NULL;

  if(argc == 5){
      num_serv = atoi(argv[1]);
      port_num = atoi(argv[2]);
      chkpt_filename = argv[3];
      vm_name = argv[4];

      printf(" num_serv = %d port_num = %d chkpt_filename = %s vm_name = %s\n", num_serv, port_num, chkpt_filename, vm_name);
      fflush(stdout);
  }
  else{
      printf(" invalid parameter \n");
      exit(1);
  }
  // Create a socket
  if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
      perror("Socket");
      exit(1);
  }
  if (setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,&true,sizeof(int)) == -1) {
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
  printf("\nCYCLIC MEM Server Waiting for client\n");
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
      printf("Received Total Pages= %" PRId64 " \n",total_pages);
      fflush(stdout);

      // Received Total Pages No.
      n=read_full(connected,&servno,sizeof(servno));
      if (n<0) {
          printf("Error Reading Total Pages");
          exit(1);
      }

      // Handle requesting command
      if(strcmp(message,REQ_CMD)==0){
          count=0 ;
          values = (mempage**) calloc (total_pages,sizeof(mempage *));
          total_pages_bck = total_pages ;//Keep for Backup data case "SEND"
//           printf("total_pages_bck = %d\n",total_pages_bck);
//	     fflush(stdout);	     
          //  Control loop for received addr, data-pages
          //  1.Recv addr from client-side
	  bytes_recieved =read_full(connected,&adr,sizeof(adr));
//	     printf("Rec.adr Before convert =%"PRIx64"\n",adr);
	  if (bytes_recieved == -1) {
              fprintf(stderr,"recv addr failed :%s\n",strerror(errno));
              exit(1);
          }
//	     printf("Rec.adr=%"PRIx64" \n",adr);
//     	     fflush(stdout);
	  flags = adr &~TARGET_PAGE_MASK ;
	  adr &= TARGET_PAGE_MASK ;
//	     printf("First After convert adr=%"PRIx64" ,flags=%"PRIx64" ,MY_EOF=%"PRIx64" ,TARGET_PAGE_MASK=%"PRIx64"\n",adr,flags,MY_EOF,TARGET_PAGE_MASK);
//           fflush(stdout);
	  while(flags != MY_EOF){
	      count++;
	      index = adr >> TARGET_PAGE_BITS ;
	      index = index/num_serv ; // define storage location
//                     printf("index/2 = %d \n",index/2);
              //  2.Recv data-page
              if(*(values+index)==NULL){
	          values[index]=(mempage*)malloc(sizeof(mempage));
                  count_allocate++;
	      }
	      bytes_recieved =read_full(connected,values[index],sizeof(mempage));
                     
	      if (bytes_recieved == -1) {
	          fprintf(stderr,"recv values failed :%s\n",strerror(errno));
	          exit(1);
	      }
//	      printf("Rec.adr of values=%"PRIx64" \n",&values[index]);		     
//     	      fflush(stdout);
	      bytes_recieved =read_full(connected,&adr,sizeof(adr));
	      if (bytes_recieved == -1) {
                  fprintf(stderr,"recv addr failed :%s\n",strerror(errno));
                  exit(1);
	      }	
     	      flags = adr &~TARGET_PAGE_MASK ;
//	      printf("flags After =%"PRIx64"\n",flags);	
//	      printf("Address before convert = %"PRIx64" ",adr);	             
//	      fflush(stdout);
	      adr &= TARGET_PAGE_MASK ;
//            printf("n:%d After convert adr=%"PRIx64" ,flags=%"PRIx64" ,MY_EOF=%"PRIx64"\n",count ,adr,flags,MY_EOF);
//	      fflush(stdout);
          } //end while MY_EOF
	  printf("count recieved pages = %d ,count allocated pages = %d \n",count,count_allocate);
     	  fflush(stdout);

          gettimeofday(&endmemsave, NULL);
          memsave_elasp = ((endmemsave.tv_sec * 1000000 + endmemsave.tv_usec)-(startsave.tv_sec * 1000000 + startsave.tv_usec));
          printf("Elasp time saving pages from VM to memory = %"PRId64" ms\n", (uint64_t)(memsave_elasp/1000));

          // write to a file
          //char chkptfilename[CHKPT_NAME_LEN];
          //sprintf(chkptfilename, "vm%d.chk\0", servno);

          printf("write to chkptfile %s\n", chkpt_filename);
          fflush(stdout);

          int file_fd; 
          if((file_fd = open(chkpt_filename, O_CREAT | O_RDWR, 0666))<0){
              printf("mem server: cannot open file error no = %d\n", errno);
              fflush(stdout);
              //exit(1);
          }
              
          for(k=0;k<total_pages;k++){
              //1.send addr
	      //2.send values
	      if(*(values+k)!=NULL){
                  pageid = (k*num_serv)+servno;
                  adr = pageid*TARGET_PAGE_SIZE; // change index to addr
                  // printf("k=%d ,adr=%"PRIx64"\n",k,adr);
                  bytes_send = write_full(file_fd,&adr,sizeof(adr));
                  if (bytes_send == -1) {
                      fprintf(stderr,"send adr failed :%s\n",strerror(errno));
                      exit(1);
                  } //end if
		  bytes_send = write_full(file_fd,values[k],sizeof(mempage));
		  if (bytes_send == -1) {
        	      fprintf(stderr,"send values[k] failed :%s\n",strerror(errno));
                      exit(1);
                  } //end if
                  count_send++; 
//                printf("n=%d k=%"PRIx64"  ,adr=%"PRIx64"\n",count_send,k,adr);
//                fflush(stdout);
              } //end if NULL
	  }    

          adr = 0; 
          adr= (adr|MY_EOF);
          bytes_send = write_full(file_fd,&adr,sizeof(adr));
          close(file_fd);

          create_restoration_script(total_pages);

          gettimeofday(&endsave, NULL);
          filesave_elasp = ((endsave.tv_sec * 1000000 + endsave.tv_usec)-(endmemsave.tv_sec * 1000000 + endmemsave.tv_usec));
          printf("Elasp time saving pages from memory to file= %"PRId64" ms\n", (uint64_t)(filesave_elasp/1000));

          printf("count send to file = %d \n",count_send);
          fflush(stdout);

      }
      else{ // RECV request
          //char chkptfilename[CHKPT_NAME_LEN];
          //sprintf(chkptfilename, "vm%d.chk\0", servno);
          // read from a file
          int file_fd; 
          if((file_fd = open(chkpt_filename, O_RDONLY))<0){
              printf("mem server: cannot open chk file for read error no = %d\n", errno);
              fflush(stdout);
              exit(1);
          }

          if(values == NULL){
	      values = (mempage**) calloc (total_pages,sizeof(mempage *));
	      total_pages_bck = total_pages ;//Keep for Backup data case "SEND"
          }
          // read contents
	  bytes_recieved =read_full(file_fd,&adr,sizeof(adr));
             
	  if (bytes_recieved == -1) {
              fprintf(stderr,"recv addr failed :%s\n",strerror(errno));
              exit(1);
          }

	  flags = adr &~TARGET_PAGE_MASK ;
	  adr &= TARGET_PAGE_MASK ;
	
	  while(flags != MY_EOF){
	      count++;
	      index = adr >> TARGET_PAGE_BITS ;
	      index = index/num_serv ;

//	      2.Recv data-page
              if(*(values+index)==NULL){
        	  values[index]=(mempage*)malloc(sizeof(mempage));
                  count_allocate++;
	      }
	      bytes_recieved =read_full(file_fd,values[index],sizeof(mempage));
                     
	      if (bytes_recieved == -1) {
	          fprintf(stderr,"recv values failed :%s\n",strerror(errno));
	          exit(1);
	      }
              bytes_recieved =read_full(file_fd,&adr,sizeof(adr));
	      if (bytes_recieved == -1) {
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
          fileret_elasp = ((endfileret.tv_sec * 1000000 + endfileret.tv_usec)-(startret.tv_sec * 1000000 + startret.tv_usec));
          printf("Elasp time retrieving pages from file = %"PRId64" ms\n", (uint64_t)(fileret_elasp/1000));

          for(k=0;k<total_pages;k++){
              //1.send addr
	      //2.send values
              if(*(values+k)!=NULL){
                  pageid = (k*num_serv)+servno;//change variable
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
          memret_elasp = ((endmemret.tv_sec * 1000000 + endmemret.tv_usec)-(endfileret.tv_sec * 1000000 + endfileret.tv_usec));
          printf("Elasp time sending mem pages to recovery VM = %"PRId64" ms\n", (uint64_t)(memret_elasp/1000));
 
      }    

  } //end while(1) 
  return 0;
}

#define SCRIPT_FILENAME_LEN	512
void create_restoration_script(uint64_t tp){

  FILE *fp;
  char script_filename[SCRIPT_FILENAME_LEN];  
  char chmod_cmd_str[SCRIPT_FILENAME_LEN];  

  if(strlen(vm_name) < (int)(SCRIPT_FILENAME_LEN/2)){
    memset(script_filename, 0, SCRIPT_FILENAME_LEN);
    sprintf(script_filename, "/home/kasidit/TLCVM/restore-vm-%s.sh", vm_name);
    sprintf(chmod_cmd_str, "chmod 755 %s", script_filename);
    printf(" %s\n", script_filename);
  }
  else{
    printf(" Error! vm_name too long %s\n", vm_name);
  }
  fp = fopen(script_filename, "w+");
  fprintf(fp, "echo \"/home/kasidit/TLCVM/restore-interleave ${1} ${2} %" PRId64 " ${3} > ${4} \" | at now \n", total_pages); 
  printf("Create script containing: ./restore-interleave ${1} ${2} %" PRId64 " ${3} > ${4} \n", total_pages); 
  fclose(fp);

  system(chmod_cmd_str); // chenge permission to 755 executable
  
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
