#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"

void deleteFromRamAndbringFromSwap(struct memPage *pg);
int fileToPhys(struct memPage* pg);
int physToFile(struct memPage* pg);
struct memPage* getMemPage();
int addPgToSwap(struct memPage* pg);
void removePgFromPhysList(struct memPage* pg);
void addPgToPhysList(struct memPage *pg);
void addPgToMemFromVa(char* addr);
void freeFilePg(struct proc* p, struct memPage* pg);

extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()

//for when process is in freevm 
int isGlobalPgdir = 0;
pde_t* globalPgdir;

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  lgdt(c->gdt, sizeof(c->gdt));
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;

  a = (char*)PGROUNDDOWN((uint)va);
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1);
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if(*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
 { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W}, // more devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (P2V(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                (uint)k->phys_start, k->perm) < 0) {
      freevm(pgdir);
      return 0;
    }
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(V2P(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  if(p == 0)
    panic("switchuvm: no process");
  if(p->kstack == 0)
    panic("switchuvm: no kstack");
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");

  pushcli();
  mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
                                sizeof(mycpu()->ts)-1, 0);
  mycpu()->gdt[SEG_TSS].s = 0;
  mycpu()->ts.ss0 = SEG_KDATA << 3;
  mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
  // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
  // forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (ushort) 0xFFFF;
  ltr(SEG_TSS << 3);
  lcr3(V2P(p->pgdir));  // switch to process's address space
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if((uint) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, P2V(pa), offset+i, n) != n)
      return -1;
  }
  return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  struct proc* p = myproc();
  char *mem;
  uint a;

  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){
    if(SELECTION != NONE){
      if(p->pid > 2){  //p isnt shell or init
        // if reached MAX_TOTAL_PAGES -> fail
        if(p->physCounter + p->fileCounter == MAX_TOTAL_PAGES){
          panic("reached max total pages - allocuvm\n");
        }
        //if RAM is full, need to swap from file
        if(p->physCounter == MAX_PSYC_PAGES){
          if(p->swapFile == 0){
            createSwapFile(p);
          }
          struct memPage* pg = getMemPage();  //get page from phys mem to swap

          if(physToFile(pg) != 0){
            panic("allocuvm failed in physToFile\n");
          }
        }
      }
    }
    mem = kalloc();
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
      cprintf("allocuvm out of memory (2)\n");
      deallocuvm(pgdir, newsz, oldsz);
      kfree(mem);
      return 0;
    }

    //after clearing space for a new page in RAM -> adding the page
    if(p->pid > 2){
      if(SELECTION != NONE){
        addPgToMemFromVa((char*)a);
      }
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  pte_t *pte;
  uint a, pa;
  struct proc* p = myproc();

  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for(; a  < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);
    if(!pte)
      a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
    else if((*pte & PTE_P) != 0){ // page in Phys-mem
      pa = PTE_ADDR(*pte); // clearing page from RAM
      if(pa == 0)
        panic("deallocuvm 1\n");
      char *v = P2V(pa);
      kfree(v);
      *pte = 0;
      if(p->pgdir == pgdir){
        if(SELECTION != NONE){ // bringing page from file instead
          struct memPage* pg;
          for(int i=0; i<MAX_TOTAL_PAGES; i++){
            pg = &p->allPages[i];
            if(pg->pageData.va == a){
              deleteFromRamAndbringFromSwap(pg);
              break;
            }
          }
        }
      }
    }
    else if(p->pgdir == pgdir && (*pte & PTE_PG)){ //page in file
      if(SELECTION != NONE){ 
        *pte = 0;
        struct memPage* pg;
        for(int i=0; i<MAX_TOTAL_PAGES; i++){
          pg = &p->allPages[i];
          if(pg->pageData.va == a){
            freeFilePg(p, pg);
            break;
          }
        }
      }
    }
  }
  return newsz;
}

void freeFilePg(struct proc* p, struct memPage* pg){
  p->freeOffsetInFile[pg->pageData.offsetIndex / PGSIZE] = 0;
  pg->pageData.state = FREE;
  pg->pageData.va = 0xFFFFFFFF;           
  pg->pageData.offsetIndex = -1;                
  p->fileCounter--;
}

void deleteFromRamAndbringFromSwap(struct memPage *pg){
  struct proc* p = myproc();
  //update page data
  pg->pageData.va = 0xFFFFFFFF;
  pg->pageData.state = FREE;
  pg->pageData.offsetIndex = -1;
  if (p->physCounter > 0){
    p->physCounter--;
  }
  //looking for page in file to swap
  if (p->fileCounter > 0){
    for (int i=0; i<MAX_TOTAL_PAGES; i++){
      if (p->allPages[i].pageData.state == FILE){
        if(fileToPhys(&p->allPages[i]) == -1){
          return;
        }
        break;
      }   
    } 
  }
}

// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
  uint i;

  if(pgdir == 0)
    panic("freevm: no pgdir");

  //other process deleting curproc va, so use his global pgdir
  globalPgdir = pgdir;
  isGlobalPgdir = 1;
  deallocuvm(pgdir, KERNBASE, 0);
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i] & PTE_P){
      char * v = P2V(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  isGlobalPgdir = 0; 
  kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;
  char *mem;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    //page in RAM || file
    if(!((*pte & PTE_P) || (*pte & PTE_PG))) 
      panic("copyuvm: page not present");
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    //page in RAM
    if(PTE_P & *pte){
      if((mem = kalloc()) == 0)
        goto bad;
      memmove(mem, (char*)P2V(pa), PGSIZE);
      if(mappages(d, (void*)i, PGSIZE, V2P(mem), flags) < 0) {
        kfree(mem);
        goto bad;
      }
    }
    //page in file
    else{
      //pte of the page in file
      pte_t *swapPte = walkpgdir(d, (void*) i, 1); 
      *swapPte = (*pte & 0xfff);
    }
  }
  return d;

bad:
  freevm(d);
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

//TASK2 AUX//
int mappagesImport(pde_t *pgdir, void *va, uint size, uint pa, int perm){
  return mappages(pgdir, va, size, pa, perm);
}


////////////////////////////////////**TASK1**///////////////////////////////////////////////////////
//for use walkpgdir in trap.c
pte_t* walkpgdirImport(pde_t *pgdir, const void *va, int alloc){
  return walkpgdir(pgdir, va, alloc);
}

//bring page from file to phys-mem (replace between them)
int pageSwap(uint va){
  struct memPage* filePage;

  // find page in allPages array
  for(int i=0; i<MAX_TOTAL_PAGES; i++){
    filePage = &myproc()->allPages[i];
    if (filePage->pageData.va == va){
      struct memPage* memPage = getMemPage();  //get page from phys mem to swap
      if(!memPage){
        //writing page from phys mem to file
        if(physToFile(memPage) < 0){
          panic("failed in pageSwap - physToFile\n");
        }
      } 
       //bringing page from file to phys-mem
      if(fileToPhys(filePage) < 0){
        panic("failed in pageSwap - fileToPhys\n");
      }
      return 0;  
    }
  }
  return -1; // page not exist
}

//writing page to file & clear ram from deleted page
int  physToFile(struct memPage* pg) {
  pte_t *pte;
  struct proc* p = myproc();
  //check if process is in freevm
  pde_t * pgdir = isGlobalPgdir ? globalPgdir : p->pgdir;

  pte = walkpgdir(pgdir, (char*)pg->pageData.va, 0);
  //write page to swap file
  if((*pte & PTE_P) == 0){
    panic("SHOULD NOT HAPPEN\n");
  }
  if (pte == 0 || addPgToSwap(pg) == -1){
    return -1;
  }

  //remove page from phys-pages list
  removePgFromPhysList(pg);
  // update page flags
  *pte =  (PTE_PG | *pte) & (~PTE_P);
  //update -1 physical pages
  myproc()->physCounter--;
  //TASK4
  p->pageTotalNumberOfPagedOut++;
  //add page to pyhsical-mem lis

  //clear page from RAM
  kfree(P2V(PTE_ADDR(*pte)));
  lcr3(V2P(pgdir));
  return 0;
}

//writing page (from argument) from swap to phys-mem
int fileToPhys(struct memPage* pg){
  pte_t* pte;
  //check if process is in freevm
  struct proc* p = myproc();
  pde_t* pgdir = isGlobalPgdir ? globalPgdir : p->pgdir;
  // Allocate one 4096-byte page of physical memory
  char* va = kalloc();

  if (pg->pageData.offsetIndex < 0 || va == 0){
    return -1;
  }

  // create pte for pg
  mappages(pgdir, (char*)pg->pageData.va, PGSIZE, V2P(va), PTE_U |PTE_W);
  // read swap to RAM
  if (readFromSwapFile(myproc(), va, pg->pageData.offsetIndex, PGSIZE) == -1){
    return -1;
  }

  if((pte = walkpgdir(pgdir, (char *) pg->pageData.va, 0)) == 0){
    panic("failed in fileToPhys\n");
  }

  
  //set flags & states
  *pte = (PTE_P | *pte) & (~PTE_PG);
  pg->pageData.state = PHYSICAL;
  pg->pageData.offsetIndex = -1;
  //TASK 3
  pg->pageData.ageCounter = SELECTION == LAPA ? 0xFFFFFFFF : 0;
  addPgToPhysList(pg);


  return 0;
}

///TASK 3 - aux function to count 1 bits in uint
//taken from https://www.geeksforgeeks.org/count-set-bits-in-an-integer/
unsigned int countSetBits(uint n) { 
    unsigned int count = 0; 
    while (n) { 
        count += n & 1; 
        n >>= 1; 
    } 
    return count; 
} 
/////

//return page to swap from phys mem -> by SELECTION page algorithm 
struct memPage* getMemPage(){
  struct proc* p = myproc();
  pde_t* pgdir = isGlobalPgdir ? globalPgdir : p->pgdir;

  //return the page to remove from mem & write to disk
  struct memPage* pg = p->physHead;
  struct memPage* tmpPg = p->physHead;
  //choose paging algo
  switch(SELECTION){
    case(NFUA): //return page with the lowest ageCounter 
      while (tmpPg != 0){
        pte_t *pte = walkpgdir(pgdir, (char*)tmpPg->pageData.va, 0);
        if(pg->pageData.ageCounter > tmpPg->pageData.ageCounter && (*pte & PTE_U) && (*pte & PTE_P)){
          pg = tmpPg;
        }
        tmpPg = tmpPg->next;
      }
      break;
    case(LAPA): //return page with the least ones at ageCounter
      while (tmpPg != 0){
        pte_t *pte = walkpgdir(pgdir, (char*)tmpPg->pageData.va, 0);
        if(!(*pte & PTE_U) || !(*pte & PTE_P)){
          tmpPg = tmpPg->next;
          continue;
        }
        int currNumOfOnes = countSetBits(pg->pageData.ageCounter);
        int nextNumOfOnes = countSetBits(tmpPg->pageData.ageCounter);
        if(currNumOfOnes > nextNumOfOnes){
          pg = tmpPg;
        } 
        else if(currNumOfOnes == nextNumOfOnes){
          if(pg->pageData.ageCounter > tmpPg->pageData.ageCounter){
            pg = tmpPg;
          }
        }
        tmpPg = tmpPg->next;
      }
      break;
    case(SCFIFO):
      while(tmpPg != 0){
        pte_t *pte = walkpgdir(pgdir, (char*)tmpPg->pageData.va, 0);
        if(!(*pte & PTE_U) || (*pte & PTE_A) || !(*pte & PTE_P)){
          //kernel or presented page
            removePgFromPhysList(tmpPg);
            addPgToPhysList(tmpPg);
            if (*pte & PTE_A){
              *pte &= (~PTE_A);
            }
            tmpPg = p->physHead;
        }
        else{ //found page to swap
          pg = tmpPg;
          break;
        }
      }
      break;
    case(AQ): //just returns the head of the list, the updates happens in trap.c
      pg = p->physHead; //oldest page
      break;
    case(NONE):
      panic("case NONE.. not sappose to get here..\n");
      
    default:
      panic("wrong paging algorithm..\n");
  }

  return pg;
}

//adding page to file
int addPgToSwap(struct memPage* pg){
  pte_t *pte;
  struct proc* p = myproc();
  pde_t* pgdir = isGlobalPgdir ? globalPgdir : p->pgdir;

  // return free offset in file
  int offset;
  for (offset= 0; offset < MAX_PSYC_PAGES+1; offset++){
    if(myproc()->freeOffsetInFile[offset] == 0){
      break;
    }
  }
  pte = walkpgdir(pgdir, (char*)pg->pageData.va , 0);

  if (!(*pte & PTE_P) || pte == 0 || offset == MAX_PSYC_PAGES+1){
    if(!(*pte & PTE_P)){
      panic("failed in addPgToSwap1.1\n");
    }

    if(pte == 0){
      panic("failed in addPgToSwap1.2\n");
    }

    if(offset == MAX_PSYC_PAGES+1){
      panic("failed in addPgToSwap1.3\n");
    }

  }
  offset *= PGSIZE;
  //writing page to file
  if (writeToSwapFile(myproc(), (char*) pg->pageData.va, offset, PGSIZE) == -1){
    panic ("failed in addPgToSwap2\n");;
  }
  //update page fields (now in file)
  p->fileCounter++;
  pg->pageData.state = FILE;
  pg->pageData.offsetIndex = offset;
  offset /= PGSIZE;
  p->freeOffsetInFile[offset] = 1;  //turning on the occupy entery in file bit
  return 0;
}

void removePgFromPhysList(struct memPage* pg){ 
  struct proc* p = myproc();

  // page to remove from physList is the head
  if(pg->prev == 0){
    p->physHead = pg->next;
    p->physHead->prev = 0;
  }
  // pg is not head of physList
  else{ 
    pg->prev->next = pg->next;
    // pg is last
    if (pg->next != 0){
      pg->next->prev = pg->prev;
    }
  }
  // zero pointers of the page
  pg->next = 0;
  pg->prev = 0;
}

//add page from phys list
void addPgToPhysList(struct memPage *pg){
  struct proc* p = myproc();
  struct memPage *tmp = p->physHead;
  if(p->physHead != 0){ //inserting page to the end of the physList
    while (tmp->next != 0){
        tmp = tmp->next;
    }
    tmp->next = pg;
    pg->next = 0;
    pg->prev = tmp;
  }
  //first page in ram -> point phys head to first page 
  else{ 
    p->physHead = pg;
    pg->next = 0;
    pg->prev = 0;
  }
}

//adding page to mem list
void addPgToMemFromVa(char* addr){
  struct proc* p = myproc();
  struct memPage *pgToAdd = &p->allPages[0];
  struct memPage *temp = p->physHead;
  //if RAM empty -> initialize it to the beggining of the pages array
  p->physHead = temp == 0 ? &p->allPages[0] : p->physHead;

  //insert new page to the back of the physList
  int i = 0; //-> for indexing free slot
  if(temp !=0){
    while(temp->next != 0){
      temp = temp->next;
    }
    //check if page already exists in allPages -> if not need to create place
    for(i=0; i<MAX_TOTAL_PAGES; i++){
      pgToAdd = &p->allPages[i];
      if(pgToAdd->pageData.va == PTE_ADDR((uint) addr)){
        break;
      }
    }
    if (i == MAX_TOTAL_PAGES){ //no entry exsist for page 
      //looking for free space for the new page
      for(i=0; i<MAX_TOTAL_PAGES; i++){
        pgToAdd = &p->allPages[i];
        if(pgToAdd->pageData.state == FREE){
          break; //found a slot
        }
      }
    }
    //connecting page to list
    pgToAdd->prev = temp; 
    temp->next = pgToAdd;
  }
  //init page data
  pgToAdd->pageData.state = PHYSICAL;
  pgToAdd->pageData.va = PTE_ADDR(addr);
  pgToAdd->pageData.offsetIndex = -1;
  pgToAdd->pageData.indexInAllPages = i;
  pgToAdd->next = 0;
  //TASK 3
  pgToAdd->pageData.ageCounter = SELECTION == LAPA ? 0xFFFFFFFF : 0;
  //
  p->physCounter++;   
}


// TASK 2: copyOnCow
pde_t* copyOnCow(pde_t *pgdir, uint sz){
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyOnCow: pte should exist");
    //page in RAM || file
    if(!((*pte & PTE_P) || (*pte & PTE_PG))) 
      panic("copyOnCow: page not present");
    
    //page in RAM
    pa = PTE_ADDR(*pte);
    if(PTE_P & *pte){
      *pte &= ~PTE_W;
      flags = PTE_FLAGS(*pte);
      if(*pte & PTE_W){ //for readonly original pages
        *pte = *pte & ~PTE_W;
        *pte = *pte | PTE_COW;
      }
      if(mappages(d, (void*)i, PGSIZE, pa, flags) < 0) {
        goto bad;
      }
      refIncrease((char*)P2V(pa)); //increase page ref count
    }
    else{
      //pte of the page in file
      pte_t *swapPte = walkpgdir(d, (void*) i, 1); 
      *swapPte = (*pte & 0xfff);
    }
  }
  
  lcr3(V2P(pgdir)); 
  return d;

bad:
  freevm(d);
  return 0;
}



//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.

