/*  This program is a part of memory server program.
    Author: kasidit chanchio   
            Department of Computer Science,
            Thammasat University, THAILAND 
            kasiditchanchio@gmail.com
*/
#include	<stdio.h>
#include	<stdlib.h>
#include	<stdint.h>
#include	<inttypes.h>
#include	<string.h>

#include        "tlc-memory-server.h"

ram_node_type	*global_mem = NULL;
ram_node_type	*current_ptr = NULL;

uint64_t	total_num_of_pages = 0;
uint64_t	global_page_size = 0;
uint64_t        current_addr = 0;
uint64_t        local_page_id = 0;

extern uint64_t base_addr; 

uint64_t 	num_page_recorded = 0;

int 	mem_service_init(uint64_t page_size);
uint8_t *get_global_mem_ptr(ram_addr_t addr);
uint8_t *add_ram_page(ram_addr_t addr);
uint64_t get_global_page_size(void);
void 	reset_global_mem_ptr(void);
uint8_t *get_next_global_mem_ptr(ram_addr_t *current_addr);
void 	mem_service_finish(void);

int mem_service_init(uint64_t page_size){
	int i;
	int ret = 0;

        total_num_of_pages = page_size;
	if((global_mem = malloc(page_size * sizeof(ram_node_type))) == NULL){
          printf("mem_service_init: cannot allocate ram nodes %d bytes\n", 
              (page_size * sizeof(ram_node_type)));
          ret = -1; 
        }
        else{
          memset(global_mem, 0, (page_size * sizeof(ram_node_type)));
	  global_page_size = 0;
        } 
	return ret;
}

uint8_t * get_global_mem_ptr(ram_addr_t addr){
        ram_addr_t page_id = (addr - base_addr) >> TARGET_PAGE_BITS; // block distribution filtering
        return (global_mem+page_id)->page ;
}

uint8_t * add_ram_page(ram_addr_t addr){
        ram_addr_t page_id = (addr - base_addr) >> TARGET_PAGE_BITS;
        (global_mem+page_id)->page = malloc(TARGET_PAGE_SIZE);
        global_page_size++;
        return (global_mem+page_id)->page ;
}

uint64_t get_global_page_size(void){
        return global_page_size;
}

// We use the two functions below to adjust addr values
// for the recovery VM.

void reset_global_mem_ptr(void){
	current_ptr = global_mem;
        // adjusting addr based on block distribution
        current_addr = base_addr; 
        local_page_id = 0;
}

uint8_t *get_next_global_mem_ptr(ram_addr_t *addr){
        uint8_t *p = NULL;
        while(local_page_id < total_num_of_pages){
            if(current_ptr->page == NULL){
                current_ptr++; 
                current_addr += TARGET_PAGE_SIZE;
                local_page_id++;
            }
            else{
                *addr = current_addr;
                p = current_ptr->page;

                current_ptr++; 
                current_addr += TARGET_PAGE_SIZE;
                local_page_id++;

                break;
            }
        }
        return p;
}

void mem_service_finish(void){
    free(global_mem);
}
