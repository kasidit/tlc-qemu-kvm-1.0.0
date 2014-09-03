/*store.c*/
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
#define MY_EOF      0x0FF
#define TARGET_PAGE_SIZE ((uint64_t) (1 << TARGET_PAGE_BITS))
#define TARGET_PAGE_MASK ~(TARGET_PAGE_SIZE - 1)

typedef struct {
  uint8_t memnode[4096];
} mempage;

typedef uint64_t ram_addr_t;
int connected;

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
  uint64_t total_pages,total_pages_bck;
  ram_addr_t  adr,k,index=0,flags,pageid;
  mempage **values;
 

  if(argc == 3){
    num_serv = atoi(argv[1]);
    port_num = atoi(argv[2]);
printf(" num_serv = %d port_num = %d\n", num_serv, port_num);
fflush(stdout);
  }
  else{
    printf(" invalid parameter \n");
    exit(1);
  }
/////////////////////////////////////////////////
// Create a socket
/////////////////////////////////////////////////        
   if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("Socket");
        exit(1);
    }
   if (setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,&true,sizeof(int)) == -1) {
        perror("Setsockopt");
        exit(1);
    }
///////////////////////////////////////////////
// Identify IP address and Port
//////////////////////////////////////////////
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_num);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    bzero(&(server_addr.sin_zero),8);
//////////////////////////////////////////////
// Bind an IP address to the socket
//////////////////////////////////////////////
    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(struct sockaddr))== -1) {
            perror("Unable to bind");
            exit(1);
    }
/////////////////////////////////////////////
// Specify a size of a request queue
/////////////////////////////////////////////
   if (listen(sock, 5) == -1) {
       perror("Listen");
       exit(1);
   }

   printf("\nTCPServer Waiting for client\n");
   fflush(stdout);
/////////////////////////////////////////////////
// Main Server Loop - accept and handle request
/////////////////////////////////////////////////
//   printf("\nServer up and running \n");
//   printf("TARGET_PAGE_SIZE=%d ,TARGET_PAGE_MASK=%"PRIx64"\n",TARGET_PAGE_SIZE,TARGET_PAGE_MASK);
   while(1)
   {
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

////////////////////////////////////////////
// Receive message request from client
///////////////////////////////////////////     
     //n=recv(connected,message,4,0);
     n=read_full(connected,message,4);
     if (n<0) {
       printf("Error Reading Request Message");
       exit(1);
     }
     printf("Received Message Request is %s ,  %d \n",message,n);
     fflush(stdout);
////////////////////////////////////////////
// Received Total Pages No.
////////////////////////////////////////////
     n=read_full(connected,&total_pages,sizeof(total_pages));
     if (n<0) {
       printf("Error Reading Total Pages");
       exit(1);
     }
     printf("Received Total Pages= %d \n",total_pages);
//     printf("MY_TARGET_PAGE_MASK=%"PRIx64"",MY_TARGET_PAGE_MASK);
     fflush(stdout);
//////////////////////////////////////////
// Received Total Pages No.
////////////////////////////////////////////
     n=read_full(connected,&servno,sizeof(servno));
     if (n<0) {
       printf("Error Reading Total Pages");
       exit(1);
     }

////////////////////////////////////////////
// Handle message request
////////////////////////////////////////////
    if(strcmp(message,"SEND")==0){
	     count=0 ;
	     values = (mempage*) calloc (total_pages,sizeof(mempage));
	     total_pages_bck = total_pages ;//Keep for Backup data case "SEND"
//             printf("total_pages_bck = %d\n",total_pages_bck);
//	     fflush(stdout);	     
//	     memset(values,NULL,sizeof(values));
///////////////////////////////////////////
// Control loop for received addr, data-pages
//          1.Recv addr from client-side
///////////////////////////////////////////
//	     bytes_recieved =recv(connected,&adr,sizeof(adr),0);
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
//             fflush(stdout);
	
	     while(flags != MY_EOF){
		     count++;
		     index = adr >> TARGET_PAGE_BITS ;
		     index = index/num_serv ;
//                     printf("index/2 = %d \n",index/2);
///////////////////////////////////////////
//	     2.Recv data-page
///////////////////////////////////////////
        	     if(*(values+index)==NULL){
	        	values[index]=(mempage*)malloc(sizeof(mempage));
                        count_allocate++;
	             }
//		     memset(values[index],0,sizeof(values));
//             	     bytes_recieved =recv(connected,values[index],sizeof(mempage),0);
		     bytes_recieved =read_full(connected,values[index],sizeof(mempage));
                     
	             if (bytes_recieved == -1) {
	                  fprintf(stderr,"recv values failed :%s\n",strerror(errno));
	                  exit(1);
	             }
//		     printf("Rec.adr of values=%"PRIx64" \n",&values[index]);		     
//     	             fflush(stdout);
//		     bytes_recieved =recv(connected,&adr,sizeof(adr),0);
		     bytes_recieved =read_full(connected,&adr,sizeof(adr));
	             if (bytes_recieved == -1) {
         	         fprintf(stderr,"recv addr failed :%s\n",strerror(errno));
                	 exit(1);
	             }	

     	   	     flags = adr &~TARGET_PAGE_MASK ;
//		     printf("flags After =%"PRIx64"\n",flags);	
//		     fflush(stdout);
//		     printf("Address before convert = %"PRIx64" ",adr);	             
		     adr &= TARGET_PAGE_MASK ;
//        	     printf("n:%d After convert adr=%"PRIx64" ,flags=%"PRIx64" ,MY_EOF=%"PRIx64"\n",count ,adr,flags,MY_EOF);
//		     fflush(stdout);

		  }//end while MY_EOF

             gettimeofday(&endmemsave, NULL);
             memsave_elasp = ((endmemsave.tv_sec * 1000000 + endmemsave.tv_usec)-(startsave.tv_sec * 1000000 + startsave.tv_usec));
             printf("Elasp time saving pages from VM to memory = %"PRId64" ms\n", (uint64_t)(memsave_elasp/1000));

	     printf("count recv = %d ,count_allocate = %d \n",count,count_allocate);
     	     fflush(stdout);
/*
	     n=0;	
	     for(k=0;k<total_pages;k++){
		if(*(values+k)!= NULL){
                     n++;
		     adr = k*TARGET_PAGE_SIZE;
		     printf("n=%d k=%"PRIx64" values[k]=%"PRIx64" ,adr=%"PRIx64"\n",n,k,values[k],adr);
		     fflush(stdout);
		 }
	     }
*/
//RECV
  	  }else
	    {	count_send=0;
		//bytes_send = write_full(connected,&total_pages_bck,sizeof(total_pages_bck));
		//if (bytes_send == -1) {
                //         fprintf(stderr,"send total_pages_bck failed :%s\n",strerror(errno));
                //         exit(1);
                //}
        	for(k=0;k<total_pages;k++){
                    //1.send addr
		    //2.send values
 		    //Memory-Server Even #0
		    if(*(values+k)!=NULL){
			pageid = (k*num_serv)+servno;//change variable
			adr = pageid*TARGET_PAGE_SIZE;
//			printf("k=%d ,adr=%"PRIx64"\n",k,adr);
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
//			printf("n=%d k=%"PRIx64"  ,adr=%"PRIx64"\n",count_send,k,adr);
//                        fflush(stdout);
		    }//end if NULL
	        }//end for k
                memserver_send_eof();

		printf("count_send = %d\n",count_send);
		fflush(stdout);
                close(connected);

                gettimeofday(&endmemret, NULL);
                memret_elasp = ((endmemret.tv_sec * 1000000 + endmemret.tv_usec)-(startret.tv_sec * 1000000 + startret.tv_usec));
                printf("Elasp time sending pages to recovery VM = %"PRId64" ms\n", (uint64_t)(memret_elasp/1000));
 
	    }

    }//end while(1) out-side
    return 0;
}
int memserver_send_eof(void){
//////////////////////////////////
// Sending addr+flag and *ptr
//////////////////////////////////
    int ret;
    uint8_t *pp;
    ram_addr_t          addr = 0;

//    printf("MY_EOF =%p\n",MY_EOF);
//    printf("SEND  addr Before add FLAGS=%"PRIx64"\n",addr);
    addr= (addr|MY_EOF);
//    printf("SEND  addr+flag=%"PRIx64"\n",addr);
    ret = write_full(connected,&addr,sizeof(addr));
//    printf("SEND MY_EOF ret=%d\n",ret);
    return ret;
}

int read_full(int fd, void *buf, size_t count){
    ssize_t ret = 0;
    ssize_t total = 0;

    while (count) {
        ret = read(fd, buf, count);
        //ret = recv(fd,buf,count,0);
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
