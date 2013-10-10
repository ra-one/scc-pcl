#ifndef SCC_H
#define SCC_H

#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>

#include "scc_config.h"
#include "debugging.h"

/* Utility functions*/
//typedef int bool;
//#define false 0
//#define true  1


#define PAGE_SIZE           (16*1024*1024)
#define LINUX_PRIV_PAGES    (20)
#define PAGES_PER_CORE      (41)
#define MAX_PAGES           (152) //instead of 192, because first 41 can not be used to map into
#define SHM_MEMORY_SIZE			0x97000000
#define SHM_ADDR            0x41000000
//#define LOCAL_SHMSIZE  			SHM_MEMORY_SIZE/num_worker
#define MEMORY_OFFSET(id) 	(id *(SHM_MEMORY_SIZE/(num_worker+num_wrapper)))


#define CORES               (NUM_ROWS * NUM_COLS * NUM_CORES)
#define IRQ_BIT             (0x01 << GLCFG_XINTR_BIT)

#define FOOL_WRITE_COMBINE  (mpbs[node_id][0] = 1)
#define SNETGLOBWAIT        (*(mpbs[0] + 2))
// workers will wait on this address to become number of workers
#define WAITWORKERS         (*(mpbs[0] + 16))
// master will write malloc address at this address so that workers can get it
#define MALLOCADDR          (mpbs[0] + 32)


#define LUT(loc, idx)       (*((volatile uint32_t*)(&luts[loc][idx])))

/* Power defines */
#define RC_MAX_FREQUENCY_DIVIDER     16  // maximum divider value, so lowest F
#define RC_MIN_FREQUENCY_DIVIDER     2   // minimum divider value, so highest F
#define RC_NUM_VOLTAGE_LEVELS        7
#define RC_GLOBAL_CLOCK_MHZ          1600
#define RC_VOLTAGE_DOMAINS           6


extern int node_id;
extern int master_id;
extern int num_worker;
extern int num_wrapper;
extern int num_mailboxes;
extern uintptr_t  *allMbox;

extern t_vcharp mbox_start_addr;

extern t_vcharp mpbs[CORES];
extern t_vcharp locks[CORES];
extern volatile int *irq_pins[CORES];
extern volatile uint64_t *luts[CORES];


static inline int min(int x, int y) { return x < y ? x : y; }

/* Flush MPBT from L1. */
static inline void flush() { __asm__ volatile ( ".byte 0x0f; .byte 0x0a;\n" );}

static inline void lock(int core) { while (!(*locks[core] & 0x01)); }

static inline void unlock(int core) { *locks[core] = 0; }

/*sync functions*/
typedef volatile struct _AIR {
        int *counter;
        int *init;
} AIR;

extern AIR atomic_inc_regs[2*CORES];

/* Power functions */

typedef struct {
float volt; 
int   VID;
int   MHz_cap; 
} triple;

typedef struct {
int current_volt_level; 
int current_freq_div; 
} dom_fv;

extern int activeDomains[6];
extern int RC_COREID[CORES];

int set_frequency_divider(int Fdiv, int *new_Fdiv, int domain);
int set_freq_volt_level(int Fdiv, int *new_Fdiv, int *new_Vlevel, int domain);

/* Support Functions */
void SCCInit(int numWorkers, int numWrapper, char *hostFile);
void SCCStop();
int  SCCGetNodeID();
int  SCCGetNodeRank();
int  SCCIsMaster();
int  SCCGetNumWrappers();
double SCCGetTime();
void atomic_incR(AIR *reg, int *value);
void atomic_decR(AIR *reg, int value);
void atomic_readR(AIR *reg, int *value);
void atomic_writeR(AIR *reg, int value);

/* malloc functions */

void SCCMallocInit(uintptr_t *addr,int numMailboxes);
void SCCMallocStop(void);
void *SCCGetlocal(void);
void *SCCGetLocalMemStart(void);
void *SCCMallocPtr(size_t size);
void SCCFreePtr(void *p);

int DCMflush();

#endif /*SCC_H*/

