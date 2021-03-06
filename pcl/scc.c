#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h> /*for uint16_t*/
#include <stdarg.h>
#include <malloc.h> /* Prototypes for __malloc_hook, __free_hook */

#include "scc.h"

#define PRT_SYNC //
#define PRT_ADR //
#define PRT_MBX //
#define USE_MALLOC_HOOK

FILE *masterFile;

//used to write SHM start address into MPB
uintptr_t  addr=0x0;
uintptr_t  *allMbox;

// global variables for the MPB, LUT, PINS, LOCKS and AIR
t_vcharp mpbs[CORES];
t_vcharp locks[CORES];
volatile int *irq_pins[CORES];
volatile uint64_t *luts[CORES];
AIR atomic_inc_regs[2*CORES];

// global variables for power management
static triple RC_V_MHz_cap[] = {
/* 0 */ {0.9, 0x90, 460},
/* 1 */ {0.9, 0x90, 598},
/* 2 */ {0.9, 0x90, 644},
/* 3 */ {1.0, 0xA0, 748},
/* 4 */ {1.1, 0xB0, 875},
/* 5 */ {1.2, 0xC0, 1024},
/* 6 */ {1.3, 0xD0, 1198}
};

// tile clock change words, assuming constant router ratio of 2 
static unsigned int RC_frequency_change_words[][3] = {
// rtr clock ratio, bits 25:8
/* NOP */ {    2,   0x00000000, 000}, /**/
/*  1 */  {    2,   0x000038e0, 000}, /* */
/*  2 */  {    2,   0x000070e1, 800}, /* 800 */
/*  3 */  {    2,   0x0000a8e2, 533}, /* 533 */
/*  4 */  {    2,   0x0000e0e3, 400}, /* 400 */
/*  5 */  {    2,   0x000118e4, 320}, /* 320 */
/*  6 */  {    2,   0x000150e5, 266}, /* 266 */
/*  7 */  {    2,   0x000188e6, 228}, /* 228 */
/*  8 */  {    2,   0x0001c0e7, 200}, /* 200 */
/*  9 */  {    2,   0x0001f8e8, 178}, /* 178 */
/* 10 */  {    2,   0x000230e9, 160}, /* 160 */
/* 11 */  {    2,   0x000268ea, 145}, /* 145 */
/* 12 */  {    2,   0x0002a0eb, 133}, /* 133 */
/* 13 */  {    2,   0x0002d8ec, 123}, /* 123 */
/* 14 */  {    2,   0x000310ed, 114}, /* 114 */
/* 15 */  {    2,   0x000348ee, 106}, /* 106 */
/* 16 */  {    2,   0x000380ef, 100}  /* 100 */
};
/*
static int RC_domains[6][4] = {
    { 0,  2, 12, 14}, { 4,  6, 16, 18},{ 8, 10, 20, 22},
    {24, 26, 36, 38}, {28, 30, 40, 42},{32, 34, 44, 46}
};*/

static const int RC_domains[6][4]={ // 4 5 7 0 1 3
  {0, 1, 6, 7},    {2, 3, 8, 9},    {4, 5, 10, 11},
  {12, 13, 18, 19},{14, 15, 20, 21},{16, 17, 22, 23}
};

//static int VCCADDR[] = {0x8410,0x8414,0x8418,0x8400,0x8404,0x840C};
// VCC values are shiftred by 1, as bug 328 on scc
//static int VCCADDR[] = {0x8404,0x8408,0x8410,0x8414,0x8418,0x8400}; // 0 1 3 4 5 7
static int VCCADDRP[] = {0x8414,0x8418,0x8400,0x8404,0x8408,0x8410}; // 4 5 7 0 1 3
static int* VCCADDRV[6];
static int PD[] = {4,5,7,0,1,3};

t_vintp fChange_vAddr[CORES/2];
t_vintp RPC_virtual_address; // only one RPC on chip
dom_fv  RC_current_val[RC_VOLTAGE_DOMAINS];
// array of physical core IDs for all participating cores, sorted by rank
int     RC_COREID[CORES];

int RC_voltage_level(int Fdiv);
int RC_set_frequency_divider(int tile_ID, int Fdiv);
unsigned int FID_word(int Fdiv, int tile_ID);
unsigned int VID_word(float voltage, int domain);
int get_divider(int tile_ID);
int getInt(float voltage);
double readVCC(int domain); // takes logical domain gives value for physical domain
static int read_current_frequency(int tileId);

// register for timer
int *tlo,*thi;
double startTime;

//register for voltages
int DVFS;
int *C3v3SCC, *V3v3SCC;

int node_id = -1;  // physical location
int node_rank = -1;  // logical location
int num_worker = -1; // number of workers including master
int num_wrapper = -1; // number of wrappers
int num_mailboxes = -1; // number of mailboxes (worker + wrappers)
int master_id = -1;
int activeDomains[6];

void SCCFill_RC_COREID(int numWorkers, int numWrapper, char *hostFile);


//////////////////////////////////////////////////////////////////////////////////////// 
// Start of Malloc / Free hooks
//////////////////////////////////////////////////////////////////////////////////////// 
#ifdef USE_MALLOC_HOOK
void *mallocStart;
static int mCount = 0;
/* Prototypes for our hooks.  */
static void my_init_hook (void);
static void *my_malloc_hook (size_t, const void *);
static void my_free_hook (void*, const void *);

typeof(__free_hook) old_free_hook;
typeof(__malloc_hook) old_malloc_hook;


/* Override initializing hook from the C library. */
void (*__malloc_initialize_hook) (void) = my_init_hook;

static void my_init_hook (void)
{
 mallocStart = malloc(sizeof(char));
 printf("mallocStart %p\n",mallocStart);


 old_malloc_hook = __malloc_hook;
 old_free_hook = __free_hook;
 __malloc_hook = my_malloc_hook;
 __free_hook = my_free_hook;
}

static void *my_malloc_hook (size_t size, const void *caller)
{
 mCount = mCount+1;
 void *result;
 /* Restore all old hooks */
 __malloc_hook = old_malloc_hook;
 __free_hook = old_free_hook;
 /* Call recursively */
 result = malloc (size);
 /* Save underlying hooks */
 old_malloc_hook = __malloc_hook;
 old_free_hook = __free_hook;
 /* printf might call malloc, so protect it too. */
 //printf ("malloc (%u) returns %p, mCount %d, caller %p\n", (unsigned int) size, result,(mCount-1),caller);
 /* Restore our own hooks */
 __malloc_hook = my_malloc_hook;
 __free_hook = my_free_hook;
 return result;
}

static void my_free_hook (void *ptr, const void *caller)
{}

static void my_free_hook1 (void *ptr, const void *caller)
{
 /* Restore all old hooks */
 __malloc_hook = old_malloc_hook;
 __free_hook = old_free_hook;
 printf ("WILL free pointer %p\n", ptr);
 /* Call recursively */
 if(ptr != NULL && mallocStart > ptr)  { printf("in free pointer %p from another CORE\n", ptr); }
 //else                                  { free (ptr); }
 /* Save underlying hooks */
 old_malloc_hook = __malloc_hook;
 old_free_hook = __free_hook;
 /* printf might call free, so protect it too. */
 printf ("free pointer %p\n", ptr);
 /* Restore our own hooks */
 __malloc_hook = my_malloc_hook;
 __free_hook = my_free_hook;
}
#endif // USE_MALLOC_HOOK
//////////////////////////////////////////////////////////////////////////////////////// 
// End of Malloc / Free hooks
//////////////////////////////////////////////////////////////////////////////////////// 
/*
void printAir1(){
  unsigned char cpu,i;
  for (i = 0; i < 49; i+=48){
    for (cpu = 0; cpu < 48; cpu++){
      printf("atomic_inc_regs[%d %p]\n",i+cpu,atomic_inc_regs[i+cpu]);
    }
  }
}

void printAir(){
  unsigned char cpu;
  for (cpu = 0; cpu < 16; cpu++){
   printf("atomic_inc_regs[%d %p]\n",cpu,(void*)atomic_inc_regs[cpu]);
  }
}
*/
void SCCInit(int numWorkers, int numWrapper, int enableDVFS, char *hostFile){
  //variables for the MPB init
  int core,size, x, y, z, address, offset,i;
  unsigned char cpu;
  
  InitAPI(0);

  masterFile = fopen("out/master.txt", "w");
  if (masterFile == NULL)fprintf(stderr, "Can't open output file for master!\n");
  
  DVFS = enableDVFS;
  
  // Timer registers
  tlo = (int *) MallocConfigReg(FPGA_BASE+0x08224);
  thi = (int *) MallocConfigReg(FPGA_BASE+0x8228);
  
  V3v3SCC = (int *) MallocConfigReg(FPGA_BASE+0x841c);
  C3v3SCC = (int *) MallocConfigReg(FPGA_BASE+0x8424);
  
  for(i=0;i<6;i++){
    VCCADDRV[i] = (int *) MallocConfigReg(FPGA_BASE+VCCADDRP[i]);
  }
  
  fprintf(stderr, "****************************\nSCC INIT at %f\n",SCCGetTime());
  
  startTime = SCCGetTime();
  
  num_worker = numWorkers;
  num_wrapper = numWrapper;
  num_mailboxes = num_worker + num_wrapper;
  
  z = ReadConfigReg(CRB_OWN+MYTILEID);
  x = (z >> 3) & 0x0f; // bits 06:03
  y = (z >> 7) & 0x0f; // bits 10:07
  z = z & 7; // bits 02:00
  node_id = PID(x, y, z);
 
  // master_id gets value in this function
  SCCFill_RC_COREID(num_worker, num_wrapper, hostFile);

  for (cpu = 0; cpu < 48; cpu++){
    x = X_PID(cpu);
    y = Y_PID(cpu);
    z = Z_PID(cpu);

    if (cpu == node_id) address = CRB_OWN;
    else address = CRB_ADDR(x, y);

    //LUT, PINS, LOCK allocation
    irq_pins[cpu] = MallocConfigReg(address + (z ? GLCFG1 : GLCFG0));
    luts[cpu] = (uint64_t*) MallocConfigReg(address + (z ? LUT1 : LUT0));
    locks[cpu] = (t_vcharp) MallocConfigReg(address + (z ? LOCK1 : LOCK0));
    
    //MPB allocation
    MPBalloc(&mpbs[cpu], x, y, z, cpu == node_id);
    
    // clear MPB
    if(SCCIsMaster()){
      for (offset=0; offset < 0x2000; offset+=8)
      *(volatile unsigned long long int*)(mpbs[cpu]+offset) = 0;
    }     
    
    //FIRST SET OF AIR
    atomic_inc_regs[cpu].counter = (int *) MallocConfigReg(air_baseE + (8*cpu));
    atomic_inc_regs[cpu].init    = (int *) MallocConfigReg(air_baseE + (8*cpu) + 4);
    
    //SECOND SET OF AIR
    atomic_inc_regs[CORES+cpu].counter = (int *) MallocConfigReg(air_baseF + (8*cpu));
    atomic_inc_regs[CORES+cpu].init    = (int *) MallocConfigReg(air_baseF + (8*cpu) + 4);
    
    if(SCCIsMaster()){
      // only one core(master) needs to call this
      *atomic_inc_regs[cpu].init = 0;
      *atomic_inc_regs[CORES+cpu].init = 0;
      unlock(cpu);
    }
  } //end for
  //***********************************************
  //LUT remapping
  
  /* Because we used all the AIR in the mailbox to run a first version,
   * atomic_inc_regs 30 is used here because to test it we never run worker Nr. 30
   * but in a proper version this should be changed back to the out-commented line above each atomic operation
   */  

  if(!SCCIsMaster()){ // should be false and only worker will wait here
    PRT_SYNC("Wait for MASTER'S LUT MAPPING!!! \n");
    while(WAITWORKERS != WAITWORKERSVAL);
    PRT_SYNC("MASTER'S LUT MAPPING DONE!!! \n");
  }
  /*
   * LUT MAPPPING WHICH TAKES UNUSED ENTRIES 0-40 FROM EACH UNUSED CORE AND 
   * MAPPS THEM INTO THE MASTER CORE, AFTERWARDS EACH CORE MAPS THE MASTER 
   * LUT ENTRY 41-192 IN HIS OWN CORE the problem of this mapping is that 
   * it can not be used for 48 cores, because in that case we don't have 
   * unused cores, therefore change it back to the version above but don't 
   * forget to check if the mapping above is correct
   */
   unsigned int val = 45138;
   for(i = 41; i<192;i++){
    //printf("LUT(%d, %d) oldEntry %zu val %zu ",node_id,i,LUT(node_id,i),val);
    LUT(node_id,i) = val++;
    //printf(" after edit: %zu\n",LUT(node_id,i));
  }
/*
  int max_pages = MAX_PAGES-1; // MAX_PAGES = 152
  
  int i, lut, copyTo, copyFrom, entryTo,entryFrom;
  unsigned char num_pages=0;
  //int copyFrm[]={4,5,16,17};
  int copyFrm[]={8,9,10,11};
  	
  for (i = 1; i < CORES / num_worker && num_pages < max_pages; i++) {
    for (lut = 0; lut < PAGES_PER_CORE && num_pages < max_pages; lut++) {
      copyTo = node_id;
      entryTo = PAGES_PER_CORE + num_pages++;
      copyFrom = copyFrm[i-1];
      entryFrom = lut;
      PRT_MBX("LUT(%d, %d) = LUT(%d, %d)\n",copyTo,entryTo,copyFrom,entryFrom);
      LUT(copyTo,entryTo) = LUT(copyFrom,entryFrom);
		}
  }
*/

  //***********************************************
  // some inits for the MPB
  flush();
  
  //***********************************************
  /*
  * synchronisation in the init state:
  * The Master maps the SHM and writes the SHM Start-address to the MPB such that each worker can read it and we can get a proper SHM
  *
  */
  if(SCCIsMaster()){ // only master executes this code
    SCCMallocInit((void *)&addr,num_mailboxes);
    PRT_ADR("addr: %p\n",(void*)addr);
    memcpy((void*)MALLOCADDR, (const void*)&addr, sizeof(uintptr_t));
  
    // this makes snet thread wait for lpel to finish
    //SNETGLOBWAIT = 1; 
    
    RPC_virtual_address = (t_vintp) MallocConfigReg(RPC_BASE);
    //fprintf(stderr,"RPCBASE %p vaddr %p\n",RPC_BASE,RPC_virtual_address);
    for (i=0; i<CORES/2; i++) {
      x = i%6; y = i/6; 
      fChange_vAddr[i] = (t_vintp) MallocConfigReg(CRB_ADDR(x,y)+TILEDIVIDER); 
    }
    // get values for current voltage and freq
    //get_divider(node_id);
    for (i=0; i<RC_VOLTAGE_DOMAINS; i++){
      //RC_current_frequency_divider = get_divider(node_id);
      //RC_current_val[i].current_volt_level = 4; //set to 1.1 default
      //RC_current_val[i].current_freq_div = 3; //set to 533MHz default
      int fdiv = read_current_frequency(RC_domains[i][0]);
      RC_current_val[i].current_freq_div = fdiv;
      RC_current_val[i].current_volt_level = RC_voltage_level(fdiv); 
    }
  }else{
    // worker waits untill it gets address to map shared memory from master
    memcpy((void*)&addr, (const void*)MALLOCADDR, sizeof(uintptr_t));
    PRT_ADR("addr: %p\n",(void*)addr);
    SCCMallocInit((void *)&addr,num_mailboxes);
  }

  //unlock(node_id);
  
  //assign mailbox address into array
  allMbox = SCCMallocPtr (sizeof(uintptr_t)*num_mailboxes);  
  uintptr_t temp = addr;
  for (i=0; i < num_mailboxes;i++){
    allMbox[i] = (void*)temp;
    temp = (void*)temp + 48;
    PRT_MBX("scc.c: allMbox[%d] %p\n",i,allMbox[i]);
  }
  
  //unlock all workers - only master executes this
  if(SCCIsMaster()){
    // set all inactive domains to minimum if DVFS is enabled
    if(DVFS) set_min_freq();
    WAITWORKERS = WAITWORKERSVAL;
  }
  FOOL_WRITE_COMBINE;
}

//--------------------------------------------------------------------------------------
// FUNCTION: free all config registers and close shared memory
//--------------------------------------------------------------------------------------
void SCCStop(){
  unsigned char cpu;
  int i,offset;
  double stopTime;
  
  FreeConfigReg((int*) air_baseE);
  FreeConfigReg((int*) air_baseF);  
  
  for (cpu = 0; cpu < 48; cpu++){
    FreeConfigReg((int*) irq_pins[cpu]);
    FreeConfigReg((int*) locks[cpu]);
    FreeConfigReg((int*) luts[cpu]);
    if(SCCIsMaster()){
    /*
      for (offset=0; offset < 0x2000; offset+=8){
        *(volatile unsigned long long int*)(mpbs[cpu]+offset) = 0;
      }      */
      MPBunalloc(&mpbs[cpu]);      
      // only master free this
      FreeConfigReg((int*) RPC_virtual_address);
      for (i=0; i<CORES/2; i++) {
        FreeConfigReg((int*) fChange_vAddr[i]);
      }
      fclose(masterFile);
      //system("chmod 666 out/*");
    }
  }
  SCCMallocStop();
  
  stopTime = SCCGetTime();
  fprintf(stderr, "************************************************************\n");
  fprintf(stderr, "\tStart Time: %f\n\tStop Time: %f\n\tTotal Runtime: %f\n",startTime,stopTime,stopTime-startTime);
  fprintf(stderr, "************************************************************\n");
  FreeConfigReg((int*) tlo);
  FreeConfigReg((int*) thi);
  FreeConfigReg((int*) V3v3SCC);
  FreeConfigReg((int*) C3v3SCC);
}

void SCCFill_RC_COREID(int numWorkers, int numWrapper, char *hostFile){
  FILE *fd;
  int np,cid;
  char * line = NULL;
  size_t len = 0;
  
  fd = fopen (hostFile,"r");
  
  // fill with invalid value
  for(np=0;np<CORES;np++) RC_COREID[np] = -1;
  
	for(np=0;np<(numWorkers+numWrapper);np++){
	  getline(&line,&len,fd);
    RC_COREID[np] = atoi(line);
    if(np == 0) master_id = RC_COREID[np]; // set master id
    // node_rank is virtual address
    if(RC_COREID[np] == node_id) node_rank = np;
	}
   
  fclose(fd);
  
  for(np=0;np<RC_VOLTAGE_DOMAINS;np++) activeDomains[np] = -1;
  for(np=0;np<(numWorkers+numWrapper);np++){
    cid = RC_COREID[np];
    if((cid >= 0 && cid <= 3) || (cid >= 12 && cid <= 15)) activeDomains[0] = 1;
    if((cid >= 4 && cid <= 7) || (cid >= 16 && cid <= 19)) activeDomains[1] = 1;
    if((cid >= 8 && cid <= 11) || (cid >= 20 && cid <= 23)) activeDomains[2] = 1;
    if((cid >= 24 && cid <= 27) || (cid >= 36 && cid <= 39)) activeDomains[3] = 1;
    if((cid >= 28 && cid <= 31) || (cid >= 40 && cid <= 43)) activeDomains[4] = 1;
    if((cid >= 32 && cid <= 35) || (cid >= 44 && cid <= 47)) activeDomains[5] = 1;
  }
}
//////////////////////////////////////////////////////////////////////////////////////// 
// Start of Power and freq functions // virtual domain is passed in
//////////////////////////////////////////////////////////////////////////////////////// 
static int read_current_frequency(int tileId){
  int i;
  unsigned int lower_bits = (*(fChange_vAddr[tileId]))&((1<<8)-1);
  unsigned int orig_bits = ((*(fChange_vAddr[tileId])) - lower_bits) >> 8;

  for(i=1; i<=16; i++) {
    if(orig_bits == RC_frequency_change_words[i][1]){
      return i;
    }
  }
  return -1;
}

double readVCC(int domain){
  //fprintf(stderr,"domain %d PD %d, physical 0x%x logical %p\n",domain,PD[domain],VCCADDRP[domain],VCCADDRV[domain]);
  unsigned int val = *(int*)(VCCADDRV[domain]);
  return (double)val * 0.0000251770;
}

void startPowerMeasurement(int start){
  //if(start) writeFpgaGrb(DVFS_CONFIG, 0x00000a80 );
  //else      writeFpgaGrb(DVFS_CONFIG, 0x00000800 );
}

void powerMeasurement(FILE *fileHand){
  //fprintf(fileHand,"%5.3f V\t%6.3f A\n", readStatus(DVFS_STATUS_U3V3SCC), readStatus(DVFS_STATUS_I3V3SCC));
  unsigned int volti = *(int*)V3v3SCC;
  unsigned int currenti = *(int*)C3v3SCC;
  double volt = (double)volti * 0.0002500000; //0.004;
  double current = (double)currenti * 0.0062490250; //0.0990099;
  
  //fprintf(fileHand,"V %f \tA %f \tT %f\n", volt, current,SCCGetTime());
  fprintf(fileHand,"V,%f,A,%f,", volt, current);
  fflush(fileHand);
  //fprintf(stderr,"V %f \tA %f\n", volt, current);
}

void set_min_freq(){
  int reqFreqDiv = 15,i,new_Fdiv,new_Vlevel;;
  
  for(i=1;i<RC_VOLTAGE_DOMAINS;i++){
    if(activeDomains[i] != 1){
      set_freq_volt_level(reqFreqDiv, &new_Fdiv, &new_Vlevel, i);
      fprintf(stderr,"domain %d PD %d is inactive, set to lowest frequency %d\n\n\n",i,PD[i],RC_frequency_change_words[reqFreqDiv][2]);
      fprintf(masterFile,"domain %d PD %d is inactive, set to lowest frequency %d\n\n\n",i,PD[i],RC_frequency_change_words[reqFreqDiv][2]);
      fflush(masterFile);
    }
  }
}

void change_freq(int inc){
  static int first=1;
  static observer_t *obs;
  
  if(!DVFS){
    fprintf(masterFile,"Frequency can not be changed, DVFS is disabled\n");
    return;
  }
  
  // do not change anything on active domain0 as it runs master
  int reqFreqDiv = -1,i,retVal;
  
  if(inc) fprintf(masterFile,"\nMaster: increase frequency\n");
  else    fprintf(masterFile,"\nMaster: decrease frequency\n");
  fflush(masterFile);
  
  for(i=1;i<RC_VOLTAGE_DOMAINS;i++){
    if(activeDomains[i] == 1){
      if(inc){// increase freq go up in table
        reqFreqDiv = RC_current_val[activeDomains[i]].current_freq_div - 1;
      } else { // decrease freq go down in table
        reqFreqDiv = RC_current_val[activeDomains[i]].current_freq_div + 1;
      }
      i = RC_VOLTAGE_DOMAINS+1;
    }
  }
  
  if (reqFreqDiv > RC_MAX_FREQUENCY_DIVIDER){
    fprintf(masterFile,"Frequency can not be changed at this point, already min\n");
    return;
  } else if (reqFreqDiv < RC_MIN_FREQUENCY_DIVIDER){
    fprintf(masterFile,"Frequency can not be changed at this point, already max\n");
    return;
  }
  
  // get observer address
  if(first && SOSIADDR !=0) { first=0; memcpy((void*)&obs, (const void*)SOSIADDR, sizeof(observer_t*));}
  //start timer for message skip in sink
  if(obs != NULL) obs->skip_count = 0;
  
  int new_Fdiv,new_Vlevel;
  
  for(i=1;i<RC_VOLTAGE_DOMAINS;i++){
    if(activeDomains[i] == 1){
      retVal = set_freq_volt_level(reqFreqDiv, &new_Fdiv, &new_Vlevel, i);
      if(retVal == -1 ) {
        fprintf(stderr,"domain %d PD %d frequency can not be changed\n\n\n",i,PD[i]);
        fprintf(masterFile,"domain %d PD %d frequency can not be changed\n",i,PD[i]);
      } else {
        fprintf(stderr,"domain %d PD %d frequency changed to %d\n\n\n",i,PD[i],RC_frequency_change_words[new_Fdiv][2]);
        fprintf(masterFile,"domain %d PD %d frequency %d time %f\n\n\n",i,PD[i],RC_frequency_change_words[new_Fdiv][2],SCCGetTime());
      }
    }
  }// change freq in obs for sink
  if(obs != NULL) obs->freq = RC_frequency_change_words[new_Fdiv][2];
}

int set_frequency_divider(int Fdiv, int *new_Fdiv, int domain) {
	int Vlevel,tile;

	// if Fdiv is under or over min/max allowed the set it to min/max
	if (Fdiv > RC_MAX_FREQUENCY_DIVIDER) Fdiv = RC_MAX_FREQUENCY_DIVIDER;
  else 
  if (Fdiv < RC_MIN_FREQUENCY_DIVIDER) Fdiv = RC_MIN_FREQUENCY_DIVIDER;
  
  // check to see if the new frequency divider is valid
  Vlevel = RC_voltage_level(Fdiv);
  
  // check current volts support requested freq
  if (RC_current_val[domain].current_volt_level < Vlevel || Vlevel < 0){
    fprintf(stderr,"Voltage leve is low for given divider\n");
    *new_Fdiv = -1;
    return(-1);
  }
  *new_Fdiv = Fdiv;
  
  // need to set frequency divider on all tiles of the domain
  for(tile=0; tile < 4; tile++) {
    //RC_set_frequency_divider(tile, Fdiv);
    RC_set_frequency_divider(RC_domains[domain][tile],Fdiv);
  }
  RC_current_val[domain].current_freq_div = Fdiv;
  return(1);
}

int set_freq_volt_level(int Fdiv, int *new_Fdiv, int *new_Vlevel, int domain) {
	int Vlevel,tile;

	// if Fdiv is under or over min/max allowed the set it to min/max
	if (Fdiv > RC_MAX_FREQUENCY_DIVIDER) Fdiv = RC_MAX_FREQUENCY_DIVIDER;
  else 
  if (Fdiv < RC_MIN_FREQUENCY_DIVIDER) Fdiv = RC_MIN_FREQUENCY_DIVIDER;
  
  // check to see if the new frequency divider is valid
  Vlevel = RC_voltage_level(Fdiv);
  
  // check volt level support requested freq
  if (Vlevel < 0){
    fprintf(stderr,"Voltage level is low for given divider\n");
    *new_Fdiv = -1;
    *new_Vlevel = -1;
    return(-1);
  }
  *new_Vlevel = Vlevel;
  *new_Fdiv = Fdiv;
  
  // if new frequency divider greater than current, adjust frequency immediately;
  // this can always be done safely if the current power state is feasible
  if (Fdiv >= RC_current_val[domain].current_freq_div){
    // need to set frequency divider on all tiles of the voltage domain
    for(tile=0; tile < 4; tile++) {
      //RC_set_frequency_divider(tile, Fdiv);
      RC_set_frequency_divider(RC_domains[domain][tile],Fdiv);
      //fprintf(stderr,"domain %d PD %d tile %02d\n",domain,PD[domain],RC_domains[domain][tile]);
    }
    RC_current_val[domain].current_freq_div = Fdiv;
  } 
  
  // write the VID word to RPC, need to send in physical domain
  unsigned int VID = VID_word(RC_V_MHz_cap[Vlevel].volt, PD[domain]);
  *RPC_virtual_address = VID;
  *RPC_virtual_address = VID;
  *RPC_virtual_address = VID;
  
  fprintf(masterFile,"wrote 0x%x to RPC for domain %d PD %d\n",VID,domain,PD[domain]);
  fflush(masterFile);
  
  // set newVolt to requested so we can check later 
  int oldVolti, newVolti, changed;
  double volRead;
  oldVolti = getInt(RC_V_MHz_cap[RC_current_val[domain].current_volt_level].volt);
  newVolti = getInt(RC_V_MHz_cap[Vlevel].volt);
  
  fprintf(masterFile,"in loop old %d new %d\n",oldVolti,newVolti);
  fflush(masterFile);
  volRead = readVCC(domain);
 
  // read status register untill value reflects change
  do{
    //volRead = readStatus(VCCADDR[domain],0.0000251770);
    volRead = readVCC(domain);
    if(newVolti > oldVolti)      { changed = newVolti <= getInt(volRead); } 
    else if(newVolti < oldVolti) { changed = newVolti >= getInt(volRead); }
    else if(newVolti == oldVolti){ changed = 1; }
  }while(!changed);
  fprintf(masterFile,"Volt int after change:  %f, %d\n",volRead,getInt(volRead));
  fflush(masterFile);
  
  RC_current_val[domain].current_volt_level = Vlevel;
  
  // if we asked for a decrease in the clock divider, apply it now, after the
  // required target voltage has been reached.
  if (Fdiv < RC_current_val[domain].current_freq_div) {
    // need to set frequency divider on all tiles of the voltage domain    
    for(tile=0; tile < 4; tile++) {
      //RC_set_frequency_divider(tile, Fdiv);
      RC_set_frequency_divider(RC_domains[domain][tile],Fdiv);
      //fprintf(stderr,"domain %d PD %d tile %02d\n",domain,PD[domain],RC_domains[domain][tile]);
    }
    RC_current_val[domain].current_freq_div = Fdiv;
  }
  return(1);
}

// takes in physical domain number
unsigned int VID_word(float voltage, int domain) {
  int VID, voltage_value;

  voltage_value = (int)(voltage/0.00625 + 0.5);
  VID =  0x10000 |( (domain << 8 ) | voltage_value);
  return VID;
}

// voltage level corresponding to the chosen frequency
int RC_voltage_level(int Fdiv) {
  int Vlevel, found, MHz;

  MHz = RC_GLOBAL_CLOCK_MHZ/Fdiv;
  found = 0;

  for (Vlevel=0; Vlevel<=RC_NUM_VOLTAGE_LEVELS; Vlevel++){
    if (RC_V_MHz_cap[Vlevel].MHz_cap >= MHz) {found=1; break;}
  }
  
  if (found)  return(Vlevel);
  else        return(-1);
}

// set frequency divider on a single tile 
int RC_set_frequency_divider(int tile_ID, int Fdiv) {
  *(fChange_vAddr[tile_ID]) = FID_word(Fdiv, tile_ID);
  //fprintf(stderr,"FID_word 0x%x written for tile %02d\n",FID_word(Fdiv, tile_ID),tile_ID);
  //fprintf(masterFile,"FID_word 0x%x %d written for tile %02d\n",FID_word(Fdiv, tile_ID),RC_frequency_change_words[Fdiv][2],tile_ID);
  return (1);
}

// generate the complete word to be written to the CRB for tile clock freq 
unsigned int FID_word(int Fdiv, int tile_ID) {
  unsigned int lower_bits = (*(fChange_vAddr[tile_ID]))&((1<<8)-1);
  return(((RC_frequency_change_words[Fdiv][1])<<8)+lower_bits);
  //return(((RC_frequency_change_words[Fdiv][1])<<8)+0xf0);
}

// get the complete word from the CRB for tile clock frequency
int get_divider(int tile_ID) {
  int step;
  unsigned int word;
  // shift to the right to get the correct bits
  word = (*(fChange_vAddr[tile_ID]))>>8;
  for (step=0; step<=16; step++) {
	  if (word==RC_frequency_change_words[step][1]) break;
  }
  
  if (word==RC_frequency_change_words[step][1]) return(step);
  else                                          return(-1);
}

int getInt(float voltage)
{
  return (int)(voltage * 10 + 0.5);
}

//////////////////////////////////////////////////////////////////////////////////////// 
// End of Power and freq functions
//////////////////////////////////////////////////////////////////////////////////////// 

//--------------------------------------------------------------------------------------
// FUNCTION: read valaue of register holding start time from FPGA
//--------------------------------------------------------------------------------------
static inline unsigned long long gtsc(void)
{   
   unsigned int lo, hi;
   lo = *tlo;
   hi = *thi;
   return (unsigned long long)hi << 32 | lo;
}


//--------------------------------------------------------------------------------------
// FUNCTION: generates time based on 125MHz system clock of the FPGA
//--------------------------------------------------------------------------------------
double SCCGetTime()
{ 
  // this is to generate time based on 125MHz system clock of the FPGA
  return ( ((double)gtsc())/(0.125*1.e9));
  // this is to generate time for 533 MHz clock
  //return ( ((double)gtsc())/(0.533*1.e9));
}

//--------------------------------------------------------------------------------------
// FUNCTION: SCCGetNodeID
//--------------------------------------------------------------------------------------
// Returns node's physical ID
//--------------------------------------------------------------------------------------
int SCCGetNodeID(void){
	return node_id;
}

//--------------------------------------------------------------------------------------
// FUNCTION: SCCGetNodeRank
//--------------------------------------------------------------------------------------
// Returns node's logical ID
//--------------------------------------------------------------------------------------
int SCCGetNodeRank(void){
	return node_rank;
}

//--------------------------------------------------------------------------------------
// FUNCTION: SCCGetNumWrappers
//--------------------------------------------------------------------------------------
// Returns number of wrappers
//--------------------------------------------------------------------------------------
int SCCGetNumWrappers(void){
	return num_wrapper;
}

//--------------------------------------------------------------------------------------
// FUNCTION: SCCGetNumCores
//--------------------------------------------------------------------------------------
// Returns number of participating cores
//--------------------------------------------------------------------------------------
int SCCGetNumCores(void){
	return num_mailboxes;
}

//--------------------------------------------------------------------------------------
// FUNCTION: SCCIsMaster
//--------------------------------------------------------------------------------------
// Returns 1 if node is master 0 otherwise
//--------------------------------------------------------------------------------------
int SCCIsMaster(void){
  // if logical address is 0 then its master
	return (node_rank == 0);
}

//--------------------------------------------------------------------------------------
// FUNCTION: atomic_inc
//--------------------------------------------------------------------------------------
// Increments an AIR register and returns its privious content
//--------------------------------------------------------------------------------------
void atomic_incR(AIR *reg, int *value)
{
  (*value) = (*reg->counter);
}

//--------------------------------------------------------------------------------------
// FUNCTION: atomic_dec
//--------------------------------------------------------------------------------------
// Decrements an AIR register and returns its privious content
//--------------------------------------------------------------------------------------
void atomic_decR(AIR *reg, int value)
{
  (*reg->counter) = value;
}

//--------------------------------------------------------------------------------------
// FUNCTION: atomic_read
//--------------------------------------------------------------------------------------
// Returns the current value of an AIR register with-out modification to AIR
//--------------------------------------------------------------------------------------
void atomic_readR(AIR *reg, int *value)
{
  (*value) = (*reg->init);
}

//--------------------------------------------------------------------------------------
// FUNCTION: atomic_write
//--------------------------------------------------------------------------------------
// Initializes an AIR register by writing a start value
//--------------------------------------------------------------------------------------
void atomic_writeR(AIR *reg, int value)
{
  (*reg->init) = value;
}

