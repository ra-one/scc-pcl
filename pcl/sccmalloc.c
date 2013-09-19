#include <fcntl.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/mman.h>


#include "scc.h"
#include <pthread.h>
//#define PRT_DBG printf
//mutex variable used to lock SCCMallocPtr, because of the possibility of different threads running on the same core
pthread_mutex_t malloc_lock;

typedef union block {
  struct {
    union block *next;
    size_t size;
  } hdr;
  uint32_t align;   // Forces proper allignment
} block_t;

unsigned char local_pages=MAX_PAGES;
int node_ID;

static void *local;
static int mem;
static block_t *freeList;

/*
 * returns the LUT-entry where an address is located in the SHM
 */
lut_addr_t SCCPtr2Addr(void *p)
{
  uint32_t offset;
  unsigned char lut;
  if (local <= p && p <= local + SHM_MEMORY_SIZE) {
    offset = (p - local) % PAGE_SIZE;
    lut = LOCAL_LUT + (p - local) / PAGE_SIZE;
  } else {
    fprintf(stderr, "Invalid pointer\n");
  }

  lut_addr_t result = {node_location, lut, offset};
  return result;
}
/*
 * returns the address to an given LUT-entry
 */
void *SCCAddr2Ptr(lut_addr_t addr)
{
  if (LOCAL_LUT <= addr.lut && addr.lut < LOCAL_LUT + local_pages) {
    return (void*) ((addr.lut - LOCAL_LUT) * PAGE_SIZE + addr.offset + local);
  } else {
    fprintf(stderr, "Invalid SCC LUT address\n");
  }

  return NULL;
}

/*
 * SCCMallocInit creates a new mapping for the SHM and sets the "addr" pointer to the beginning address of this SHM
 */

void SCCMallocInit(uintptr_t *addr,int numMailboxes)
{
  node_ID= SCCGetNodeID();
  // Open driver device "/dev/rckdyn011" to map memory in write-through mode 
  //mem = open("/dev/rckdcm", O_RDWR|O_SYNC);
  mem = open("/dev/rckdyn010", O_RDWR|O_SYNC);
  PRT_DBG("mem: %i\n", mem);
  if (mem < 0) {
		fprintf(stderr, "Opening /dev/rckdyn011 failed!\n");
  }	

  /*
   * create a new mapping for the SHM
   * if the addr ptr. is unset then the calling node is the MASTER and has to create the mapping and set the start-address
   * if the addr ptr. is set then the calling node is a WORKER and just has to map the memory to a fixed start-address gotten from the MASTER
   *
   */
	if (*addr==0x0){
		PRT_DBG("MASTER MMAP\n\n");
		local = mmap(NULL, 		SHM_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mem, LOCAL_LUT << 24);
		
		if (local == NULL) fprintf(stderr, "Couldn't map memory!\n");
		else	munmap(local, SHM_MEMORY_SIZE);
		
		local = mmap((void*)local, 	SHM_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, mem, LOCAL_LUT << 24);
		
		if (local == NULL) fprintf(stderr, "Couldn't map memory!\n");
		*addr=local;
  }else{
		PRT_DBG("WORKER MMAP\n\n");
		local=*addr;	
		local = mmap((void*)local,     	SHM_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, mem, LOCAL_LUT << 24);
		if (local == NULL) fprintf(stderr, "Couldn't map memory!");
  }  

  PRT_DBG("addr: %p\nlocal: %p\nMEMORY_OFFSET(%d): %u\n",*addr,local,node_ID,MEMORY_OFFSET(node_ID));
  

  //calculate the start-address in the SHM, depending on the max. number of participating WORKERS and the ID of the calling WORKER
  if(node_ID == 0){
    freeList = local+(48*numMailboxes);
  } else {
    freeList = local+MEMORY_OFFSET(node_ID);
  }
  PRT_DBG("freelist address: %p\n",freeList);
  	
	/*
  lut_addr_t *addr_t=(lut_addr_t*)malloc(sizeof(lut_addr_t));
  *addr_t= SCCPtr2Addr(freeList);
  PRT_DBG("LUT-entry of freelist:		%d\n",addr_t->lut);
  PRT_DBG("freelist's LUT offset: 		%u\n",addr_t->offset);
	*/
	
  freeList->hdr.next = freeList;
  freeList->hdr.size = SHM_MEMORY_SIZE / sizeof(block_t);

  //init mutex variable for the SCCMallocPtr function
  pthread_mutex_init(&malloc_lock, NULL);
}

void *SCCGetLocal(void){
	return local;
}

void SCCMallocStop(void)
{
  munmap(local, SHM_MEMORY_SIZE);
  close(mem);
}

/*
 * SCCMallocPtr is used to allocate memory in the SHM
 */

void *SCCMallocPtr(size_t size)
{
  size_t nunits;
  block_t *curr, *prev, *new;
	pthread_mutex_lock(&malloc_lock);

  if (freeList == NULL) fprintf(stderr, "Couldn't allocate memory freelist is NULL!\n");
  prev = freeList;
  curr = prev->hdr.next;
  nunits = (size + sizeof(block_t) - 1) / sizeof(block_t) + 1;
  PRT_DBG("SCCMallocPtr is called for size: %zu, nunits %zu\n",size,nunits);
  do {
		/* the following debugging printout is very useful to check if there is a Problem 
		 * with the memory allocation, usually forced by a not allowed write to the SHM 
		 * either by a normal malloc or a manual write to an address in the SHM
		 */
		 //PRT_DBG("\ncurr->hdr.size:					%zu\n",curr->hdr.size);
     //size and next will always be same as there is only one element in free list
     PRT_DBG("prev->hdr.size %zu,prev->hdr.next %p, curr->hdr.size %zu, curr->hdr.next %p\n",prev->hdr.size,prev->hdr.next,curr->hdr.size,curr->hdr.next);
			if (curr->hdr.size >= nunits){
				if (curr->hdr.size == nunits){
					if (prev == curr){
						PRT_DBG("SET prev TO NULL in malloc\n");
						prev = NULL;
					}else{
						prev->hdr.next = curr->hdr.next;
					}
				} else if (curr->hdr.size > nunits){
					new = curr + nunits;
					*new = *curr;
					new->hdr.size -= nunits;
					curr->hdr.size = nunits;
					if (prev == curr) prev = new;
						prev->hdr.next = new;
				}
				freeList = prev;
				pthread_mutex_unlock(&malloc_lock);
        //printf("SCCMalloc: returned %p at time: %f of size: %d\n",(void*) (curr + 1),SCCGetTime(),size);
				return (void*) (curr + 1);
			}
		} while (curr != freeList && (prev = curr, curr = curr->hdr.next));

		pthread_mutex_unlock(&malloc_lock);

		fprintf(stderr, "Couldn't allocate memory: not enough available!\n");
		return NULL;
}

/*
 * SCCFreePtr is used to free memory in the SHM
 */
void SCCFreePtr(void *p)
{}
void SCCFreePtr1(void *p)
{
  block_t *block = (block_t*) p - 1,
          *curr = freeList;

  if (freeList == NULL) {
    freeList = block;
    freeList->hdr.next = freeList;
    return;
  }

  while (!(block > curr && block < curr->hdr.next)) {
    if (curr >= curr->hdr.next && (block > curr || block < curr->hdr.next)) break;
    curr = curr->hdr.next;
  }


  if (block + block->hdr.size == curr->hdr.next) {
    block->hdr.size += curr->hdr.next->hdr.size;
    if (curr == curr->hdr.next) block->hdr.next = block;
    else block->hdr.next = curr->hdr.next->hdr.next;
  } else {
    block->hdr.next = curr->hdr.next;
  }

  if (curr + curr->hdr.size == block) {
    curr->hdr.size += block->hdr.size;
    curr->hdr.next = block->hdr.next;
  } else {
    curr->hdr.next = block;
  }

  freeList = curr;
}


/*
 * used to flush the whole L2-cache
 */
int DCMflush() {
   //flushes the whole L2 cache
   write(mem,0,65536);
//   write(mem,0,0);
   return 1;
}


