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

#define B_OFFSET            64
#define FOOL_WRITE_COMBINE  (mpbs[node_location_phy][0] = 1)
#define START(i)            (*((volatile uint16_t *) (mpbs[i] + B_OFFSET)))
#define END(i)              (*((volatile uint16_t *) (mpbs[i] + B_OFFSET + 2)))
#define HANDLING(i)         (*(mpbs[i] + B_OFFSET + 4))
#define WRITING(i)          (*(mpbs[i] + B_OFFSET + 5))
#define B_START             (B_OFFSET + 32)
#define SNETGLOBWAIT        (*(mpbs[0] + B_START + 2))
#define B_SIZE              (MPBSIZE - B_START)
#define M_START(i)          (mpbs[i] + B_START + (*((volatile uint16_t *) (mpbs[i] + B_OFFSET + 2))));

#define LUT(loc, idx)       (*((volatile uint32_t*)(&luts[loc][idx])))

#define LUT_MEMORY_DOMAIN_OFFSET 6
#define AIR_LUT_SYNCH_VALUE 1
#define AIR_MBOX_SYNCH_VALUE 0
#define LOCAL_LUT   0x29

/* Power defines */
#define RC_MAX_FREQUENCY_DIVIDER     16  // maximum divider value, so lowest F
#define RC_MIN_FREQUENCY_DIVIDER     2   // minimum divider value, so highest F
#define RC_NUM_VOLTAGE_LEVELS        7
#define RC_GLOBAL_CLOCK_MHZ          1600
#define RC_VOLTAGE_DOMAINS           6


extern int node_location_phy;
extern int master_ID;
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

void cpy_mpb_to_mem(int node, void *dst, int size);
void cpy_mem_to_mpb(int node, void *src, int size);
void cpy_mailbox_to_mpb(void *dest,void *src, size_t count);

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
void SCCInit(int masterNode, int numWorkers, int numWrapper, char *hostFile);
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

typedef struct {
  unsigned char node, lut;
  uint32_t offset;
} lut_addr_t;

lut_addr_t SCCPtr2Addr(void *p);
void *SCCAddr2Ptr(lut_addr_t addr);

void SCCMallocInit(uintptr_t *addr,int numMailboxes);
void SCCMallocStop(void);
void *SCCGetlocal(void);
void *SCCGetLocalMemStart(void);
void *SCCMallocPtr(size_t size);
void SCCFreePtr(void *p);

void SCCStartDynamicMode();
int SCCIsDynamicMode();

int DCMflush();

#endif /*SCC_H*/

