#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h> /*for uint16_t*/
#include <stdarg.h>

#include "scc.h"
#include "RCCE_memcpy.c"

//#define printfM //
#define printfM //
#define PRT_DBG1 //

//used to write SHM start address into MPB
uintptr_t  addr=0x0;
uintptr_t  *allMbox;
t_vcharp  mbox_start_addr=0x0;

// global variables for the MPB, LUT, PINS, LOCKS and AIR
t_vcharp mpbs[CORES];
t_vcharp locks[CORES];
volatile int *irq_pins[CORES];
volatile uint64_t *luts[CORES];
AIR atomic_inc_regs[2*CORES];

// global variables for power management
static triple RC_V_MHz_cap[] = {
/* 0 */ {0.8, 0x80, 460},
/* 1 */ {0.8, 0x80, 598},
/* 2 */ {0.9, 0x90, 644},
/* 3 */ {1.0, 0xA0, 748},
/* 4 */ {1.1, 0xB0, 875},
/* 5 */ {1.2, 0xC0, 1024},
/* 6 */ {1.3, 0xD0, 1198}
};

// tile clock change words, assuming constant router ratio of 2 
static unsigned int RC_frequency_change_words[][2] = {
// rtr clock ratio, bits 25:8
/* NOP */ {    2,   0x00000000}, /**/
/*  1 */  {    2,   0x000038e0}, /* */
/*  2 */  {    2,   0x000070e1}, /* 800 */
/*  3 */  {    2,   0x0000a8e2}, /* 533 */
/*  4 */  {    2,   0x0000e0e3}, /* 400 */
/*  5 */  {    2,   0x000118e4}, /* 320 */
/*  6 */  {    2,   0x000150e5}, /* 266 */
/*  7 */  {    2,   0x000188e6}, /* 228 */
/*  8 */  {    2,   0x0001c0e7}, /* 200 */
/*  9 */  {    2,   0x0001f8e8}, /* 178 */
/* 10 */  {    2,   0x000230e9}, /* 160 */
/* 11 */  {    2,   0x000268ea}, /* 145 */
/* 12 */  {    2,   0x0002a0eb}, /* 133 */
/* 13 */  {    2,   0x0002d8ec}, /* 123 */
/* 14 */  {    2,   0x000310ed}, /* 114 */
/* 15 */  {    2,   0x000348ee}, /* 106 */
/* 16 */  {    2,   0x000380ef}  /* 100 */
};

static int RC_domains[6][4] = {
    { 0,  2, 12, 14}, { 4,  6, 16, 18},{ 8, 10, 20, 22},
    {24, 26, 36, 38}, {28, 30, 40, 42},{32, 34, 44, 46}
};

static int VCCADDR[] = {0x8410,0x8414,0x8418,0x8400,0x8404,0x840C};
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

int node_location = -1;
int node_rank = -1;
int num_worker = -1;
int num_wrapper = -1;
int num_mailboxes = -1;
int master_ID = -1;
int dynamicMode = 0;
int activeDomains[6];

//variables for the AIR init
int *air_baseE, *air_baseF;

void SCCInit(int masterNode, int numWorkers, int numWrapper, char *hostFile){
  //variables for the MPB init
  int core,size, x, y, z, address;
  unsigned char cpu, num_pages;

  InitAPI(0);
  
  num_worker = numWorkers;
  master_ID = masterNode;
  num_wrapper = numWrapper;
  num_mailboxes = num_worker + num_wrapper;
  
  z = ReadConfigReg(CRB_OWN+MYTILEID);
  x = (z >> 3) & 0x0f; // bits 06:03
  y = (z >> 7) & 0x0f; // bits 10:07
  z = z & 7; // bits 02:00
  node_location = PID(x, y, z);
  
  SCCFill_RC_COREID(num_worker, hostFile);

  air_baseE = (int *) MallocConfigReg(FPGA_BASE + 0xE000);
  air_baseF = (int *) MallocConfigReg(FPGA_BASE + 0xF000);

  for (cpu = 0; cpu < 48; cpu++){
    x = X_PID(cpu);
    y = Y_PID(cpu);
    z = Z_PID(cpu);

    if (cpu == node_location) address = CRB_OWN;
    else address = CRB_ADDR(x, y);

    //LUT, PINS, LOCK allocation
    irq_pins[cpu] = MallocConfigReg(address + (z ? GLCFG1 : GLCFG0));
    luts[cpu] = (uint64_t*) MallocConfigReg(address + (z ? LUT1 : LUT0));
    locks[cpu] = (t_vcharp) MallocConfigReg(address + (z ? LOCK1 : LOCK0));
    
    //MPB allocation
    MPBalloc(&mpbs[cpu], x, y, z, cpu == node_location);

    //FIRST SET OF AIR
    atomic_inc_regs[cpu].counter = air_baseE + 2*cpu;
    atomic_inc_regs[cpu].init 	 = air_baseE + 2*cpu + 1;

    //SECOND SET OF AIR
    atomic_inc_regs[CORES+cpu].counter 	= air_baseF + 2*cpu;
    atomic_inc_regs[CORES+cpu].init 	= air_baseF + 2*cpu + 1;
    if(node_location == master_ID){
      // only one core(master) needs to call this
      *atomic_inc_regs[cpu].init = 0;
      *atomic_inc_regs[CORES+cpu].init = 0;
    }
  } //end for

  //***********************************************
  //LUT remapping

  num_pages = PAGES_PER_CORE - LINUX_PRIV_PAGES;
  int max_pages = MAX_PAGES-1;
  int i, lut, origin;
  
  /* Because we used all the AIR in the mailbox to run a first version,
   * atomic_inc_regs 30 is used here because to test it we never run worker Nr. 30
   * but in a proper version this should be changed back to the out-commented line above each atomic operation
   */  

  if(node_location != master_ID){
    int value=-1;
    PRT_DBG1("Wait for MASTER'S LUT MAPPING!!! \n");
    while(value != num_worker){
      //atomic_readR(&atomic_inc_regs[master_ID],&value);
      //atomic_readR(&atomic_inc_regs[master_ID],&value);
      memcpy((void*)&value, (const void*)mpbs[master_ID]+8, sizeof(int));
    }
    PRT_DBG("value= %d\n",value);
    PRT_DBG1("AIR==1 -> MASTER has finished LUT mapping.\n");
  }
  /*
   * LUT MAPPPING WHICH TAKES UNUSED ENTRIES 0-40 FROM EACH UNUSED CORE AND 
   * MAPPS THEM INTO THE MASTER CORE, AFTERWARDS EACH CORE MAPS THE MASTER 
   * LUT ENTRY 41-192 IN HIS OWN CORE the problem of this mapping is that 
   * it can not be used for 48 cores, because in that case we don't have 
   * unused cores, therefore change it back to the version above but don't 
   * forget to check if the mapping above is correct
   */
	num_pages=0;
  int entryNo;
	for (i = 1; i < CORES / num_worker && num_pages < max_pages; i++) {
    for (lut = 0; lut < PAGES_PER_CORE && num_pages < max_pages; lut++) {
      LUT(node_location, PAGES_PER_CORE + num_pages++) = LUT(i * num_worker, lut);
      entryNo = PAGES_PER_CORE+(num_pages-1);
    	//printf("Copy to node %d's LUT entry Nr: %d / 0x%x from node %d's LUT entry Nr: %d / 0x%x. Num_pages: %i, Max_pages: %i\n",
        //      node_location, entryNo,entryNo, i*num_worker,lut, lut, num_pages-1, max_pages);	
		}
  }

  //***********************************************
  // some inits for the MPB
  flush();
  //false = 0, true = 1
  START(node_location) = 0;
  END(node_location) = 0;
  /* Start with an initial handling run to avoid a cross-core race. */
  HANDLING(node_location) = 1;
  WRITING(node_location) = 0;

  //***********************************************
  /*
  * synchronisation in the init state:
  * The Master maps the SHM and writes the SHM Start-address to the MPB such that each worker can read it and we can get a proper SHM
  *
  */
  if(node_location == master_ID){
    SCCMallocInit((void *)&addr,num_mailboxes);
    PRT_DBG1("addr: %p\n",(void*)addr);
    memcpy((void*)mpbs[master_ID]+16, (const void*)&addr, sizeof(uintptr_t));
    //memcpy((void*)mpbs[master_ID]+8, (const void*)&num_worker, sizeof(int));
    //cpy_mem_to_mpb(0, (void *)&addr, sizeof(uintptr_t));	
    //atomic_writeR(&atomic_inc_regs[master_ID],num_worker);
    
    RPC_virtual_address = (t_vintp) MallocConfigReg(RPC_BASE);
    for (i=0; i<CORES/2; i++) {
      x = i%6; y = i/6; 
      fChange_vAddr[i] = (t_vintp) MallocConfigReg(CRB_ADDR(x,y)+TILEDIVIDER); 
    }
    // get values for current voltage and freq
    //get_divider(node_location);
    for (i=0; i<RC_VOLTAGE_DOMAINS; i++){
        //RC_current_frequency_divider = get_divider(node_location);
        RC_current_val[i].current_volt_level = 4; //set to 1.1 default
        RC_current_val[i].current_freq_div = 3; //set to 533MHz default
    }
  }else{
    /*
     * READING AND WRITING BACK AFTERWARDS BECAUSE OF POINTER MOVING IN CPY_MPB_TO_MEM
     * solve this case, by using a write to a fixed unused MPB address
     */
    
    //cpy_mpb_to_mem(0, (void *)&addr, sizeof(uintptr_t));
    //cpy_mem_to_mpb(0, (void *)&addr, sizeof(uintptr_t));
    memcpy((void*)&addr, (const void*)mpbs[master_ID]+16, sizeof(uintptr_t));
    PRT_DBG1("addr: %p\n",(void*)addr);
    SCCMallocInit((void *)&addr,num_mailboxes);
  }

  //***********************************************
  //FOOL_WRITE_COMBINE;
  unlock(node_location);
  
  //assign mailbox address into array
  allMbox = SCCMallocPtr (sizeof(uintptr_t)*num_mailboxes);  
  uintptr_t temp = addr;
  for (i=0; i < num_mailboxes;i++){
    allMbox[i] = (void*)temp;
    temp = (void*)temp + 48;
    printfM("scc.c: allMbox[%d] %p\n",i,allMbox[i]);
  }
  
  //unlock all workers
  if(node_location == master_ID){
    memcpy((void*)mpbs[master_ID]+8, (const void*)&num_worker, sizeof(int));
  }
  FOOL_WRITE_COMBINE;
  mbox_start_addr = M_START(node_location);	
}

//--------------------------------------------------------------------------------------
// FUNCTION: free all config registers and close shared memory
//--------------------------------------------------------------------------------------
void SCCStop(){
  unsigned char cpu;
  int i;
  
  FreeConfigReg((int*) air_baseE);
  FreeConfigReg((int*) air_baseF);  
  
  for (cpu = 0; cpu < 48; cpu++){
    FreeConfigReg((int*) irq_pins[cpu]);
    FreeConfigReg((int*) locks[cpu]);
    FreeConfigReg((int*) luts[cpu]);
    MPBunalloc(&mpbs[cpu]);
  }
  if(node_location == master_ID){
    FreeConfigReg((int*) RPC_virtual_address);
    for (i=0; i<CORES/2; i++) {
      FreeConfigReg((int*) fChange_vAddr[i]);
    }
  }
  SCCMallocStop();
}

SCCFill_RC_COREID(int numWorkers, char *hostFile){
  FILE *fd;
  int np,cid;
  char * line = NULL;
  size_t len = 0;
  
  fd = fopen (hostFile,"r");
  
  // fill with invalid value
  for(np=0;np<CORES;np++) RC_COREID[np] = -1;
  
	for(np=0;np<numWorkers;np++){
	  getline(&line,&len,fd);
    RC_COREID[np] = atoi(line);
    // node_rank is virtual address
    if(RC_COREID[np] == node_location) node_rank = np;
	}
   
  fclose(fd);
  
  printfM("scc: my location: %d, my rank : %d\n",node_location,node_rank);
  
  for(np=0;np<RC_VOLTAGE_DOMAINS;np++) activeDomains[np] = -1;
  for(np=0;np<numWorkers;np++){
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
  double temp,newVolt;

	// if Fdiv is under or over min/max allowed the set it to min/max
	if (Fdiv > RC_MAX_FREQUENCY_DIVIDER) Fdiv = RC_MAX_FREQUENCY_DIVIDER;
  else 
  if (Fdiv < RC_MIN_FREQUENCY_DIVIDER) Fdiv = RC_MIN_FREQUENCY_DIVIDER;
  
  // check to see if the new frequency divider is valid
  Vlevel = RC_voltage_level(Fdiv);
  
  // check volt level support requested freq
  if (Vlevel < 0){
    fprintf(stderr,"Voltage leve is low for given divider\n");
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
    set_frequency_divider(Fdiv, new_Fdiv,domain);
  } 
  
  // set newVolt to requested so we can check later 
  newVolt = RC_V_MHz_cap[Vlevel].volt;
  
  // write the VID word to RPC, need to send in physical domain
  *RPC_virtual_address = VID_word(RC_V_MHz_cap[Vlevel].volt, PD[domain]);
  
  //TODO: code for change to take effect then do next domain
  // read status register untill value reflects change
  /*
  do{
    temp = readStatus(VCCADDR[domain],0.0000251770);
  }while(temp < )
  */
  
  RC_current_val[domain].current_volt_level = Vlevel;
  
  // if we asked for a decrease in the clock divider, apply it now, after the
  // required target voltage has been reached.
  if (Fdiv < RC_current_val[domain].current_freq_div) {
    // need to set frequency divider on all tiles of the voltage domain    
    set_frequency_divider(Fdiv, new_Fdiv,domain);   
  }
  return(1);
}

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
  //printf("FID_word 0x%x written for tile %d at address %p\n",(int *)(FID_word(Fdiv, tile_ID)),tile_ID,fChange_vAddr[tile_ID]);
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
//////////////////////////////////////////////////////////////////////////////////////// 
// End of Power and freq functions
//////////////////////////////////////////////////////////////////////////////////////// 

//--------------------------------------------------------------------------------------
// FUNCTION: read valaue of register holding start time from FPGA
//--------------------------------------------------------------------------------------
static inline unsigned long long gtsc(void)
{
   unsigned int lo, hi;
   lo = ReadConfigReg(FPGA_BASE+0x08224);
   hi = ReadConfigReg(FPGA_BASE+0x8228);
   return (unsigned long long)hi << 32 | lo;
   //return (unsigned long long) ReadConfigReg(FPGA_BASE+0x8228) << 32 | ReadConfigReg(FPGA_BASE+0x08224);
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
	return node_location;
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
// FUNCTION: SCCIsMaster
//--------------------------------------------------------------------------------------
// Returns 1 if node is master 0 otherwise
//--------------------------------------------------------------------------------------
int SCCIsMaster(void){
	return (node_location == master_ID);
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


//////////////////////////////////////////////////////////////////////////////////////// 
// Deprecated  functions
//////////////////////////////////////////////////////////////////////////////////////// 

//--------------------------------------------------------------------------------------
// FUNCTION: does not need anymore, sets flag dynamic task in snet/lpel can be created now
//--------------------------------------------------------------------------------------
void SCCStartDynamicMode(){
  int i = 1;
	memcpy((void*)mpbs[master_ID]+32, (const void*)&i, sizeof(int));
  FOOL_WRITE_COMBINE;
  flush();
  dynamicMode = 1; 
}

//--------------------------------------------------------------------------------------
// FUNCTION: does not need anymore, snet/lpel check if dynamic task mode is enabled
//--------------------------------------------------------------------------------------
int SCCIsDynamicMode(){
  if (dynamicMode != 1){
   int i = -1;
   flush();
   memcpy((void*)&i, (const void*)mpbs[master_ID]+32, sizeof(int));
   if (i == 1) dynamicMode = i;
  }
  return dynamicMode;
}

//--------------------------------------------------------------------------------------
// FUNCTION: cpy_mailbox_to_mpb
//--------------------------------------------------------------------------------------
// copy struct pointed by source to M_START and return M_START in dest
//--------------------------------------------------------------------------------------
void cpy_mailbox_to_mpb(void *dest,void *src, size_t size)
{
    memcpy_put((void*) mbox_start_addr, src, size);
    dest = mbox_start_addr;
    PRT_DBG("\n****************************mbox Cpy dest %p, start_addr %p\n\n",dest,mbox_start_addr);
    PRT_DBG("memcpy_put: size %d copying at %p\n",size,mbox_start_addr);
    //mbox_start_addr = (t_vcharp)((char*)mbox_start_addr + size);
    mbox_start_addr += size;
    FOOL_WRITE_COMBINE;
    PRT_DBG("memcpy_put: mbox_start_addr after copy %p\n\n",mbox_start_addr);
   
}

void cpy_mpb_to_mem(int node, void *dst, int size)
{
  int start, end, cpy;

  flush();
  start = START(node);
  end = END(node);

  while (size) {
    if (end < start) cpy = min(size, B_SIZE - start);
    else cpy = size;

    flush();
    memcpy(dst, (void*) (mpbs[node] + B_START + start), cpy);
    start = (start + cpy) % B_SIZE;
    dst = ((char*) dst) + cpy;
    size -= cpy;
  }

  flush();
  START(node) = start;
  FOOL_WRITE_COMBINE;
}

void cpy_mem_to_mpb(int node, void *src, int size)
{
  int start, end, free;

  if (size >= B_SIZE) {
    fprintf(stderr,"Message to big!\n");
    exit(3);
  }

  flush();
  WRITING(node) = 1; //false = 0, true = 1
  FOOL_WRITE_COMBINE;

  while (size) {
    flush();
    start = START(node);
    end = END(node);

    if (end < start) free = start - end - 1;
    else free = B_SIZE - end - (start == 0 ? 1 : 0);
    free = min(free, size);

    if (!free) {
      unlock(node);
      usleep(1);
      lock(node);
      continue;
    }

		PRT_DBG("memcpy_put:\n node: %d B_START+end: %d src: %s size: %d\n", node,B_START + end,src,free);
		memcpy_put((void*) (mpbs[node] + B_START + end), src, free);

    flush();
    size -= free;
    src += free;
    END(node) = (end + free) % B_SIZE;
    WRITING(node) = 0; //false = 0, true = 1
    FOOL_WRITE_COMBINE;
  }
}
