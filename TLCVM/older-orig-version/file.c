#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

int create_chkpt_file(char *pfix, char *cur_checkpoint_name);

int create_chkpt_file(char *pfix, char *cur_checkpoint_name){
    int  fd; 
    char *chkpt_filename = NULL;
    const char namesuffix = "/chkpt.file";

    int pfix_len = strlen(pfix);
    int namesuffix_len = strlen(namesuffix);
    int chkpt_filename_len = strlen(cur_checkpoint_name);
    
    chkpt_filename =  
        (char *)malloc(sizeof(uint8_t)*(pfix_len+chkpt_filename_len+namesuffix_len+2));
    sprintf(chkpt_filename, "%s.%s\0", pfix, cur_checkpoint_name);
    if((mkdir(chkpt_filename, 0755))<0){

    }

    sprintf(chkpt_filename, "%s.%s%s\0", pfix, cur_checkpoint_name, namesuffix);

    if((fd = open(chkpt_filename, O_CREATE | O_EXCL | O_RDWR))<0){
        printf("mem server: cannot create file %s error no = %d\n", chkpt_filename, errno);
        fflush(stdout);
        free(chkpt_filename);
        return fd;
    }
    free(chkpt_filename);
    return fd;
}

int create_chkpt_meta_file(char *pfix, char *cur_checkpoint_name){
    int  fd; 
    char *chkpt_metaname = NULL;
    const char metasuffix = "/chkpt.meta";

    int pfix_len = strlen(pfix);
    int metasuffix_len = strlen(metasuffix);
    int chkpt_metaname_len = strlen(cur_checkpoint_name);
    
    chkpt_metaname = 
        (char *)malloc(sizeof(uint8_t)*(pfix_len+chkpt_metaname_len+metasuffix_len+2));
    sprintf(chkpt_metaname, "%s.%s%s\0", pfix, cur_checkpoint_name, metasuffix);

    if((fd = open(chkpt_metaname, O_CREATE | O_EXCL | O_RDWR))<0){
        printf("mem server: cannot create meta file %s error no = %d\n", chkpt_metaname, errno);
        fflush(stdout);
        free(chkpt_metaname);
        return fd;
    }
    free(chkpt_metaname);
    return fd;
}

int load_chkpt_file(void){

}

int open_registry_file(void){

}
