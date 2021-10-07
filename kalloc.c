// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file
                   // defined by the kernel linker script in kernel.ld


//TASK 4: aux counters               
uint freePgFrameCounter;
uint totalPgFrameCounter;

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  int use_lock;
  struct run *freelist;
  int refs[PHYSTOP/PGSIZE];   //TASK 2: array of ref counters (each cell holds counter of page refs for i'th page)
} kmem;

// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.
void
kinit1(void *vstart, void *vend)
{
  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 0;
  freerange(vstart, vend);
  // TASK 4: initilize total page frames inside the va of the kernel - part 1
  uint virtualStartAddr = PGROUNDUP((uint) vstart);
  uint virtualEndAddr = PGROUNDDOWN((uint) vend);
  totalPgFrameCounter = (virtualEndAddr - virtualStartAddr) / PGSIZE;
}

void
kinit2(void *vstart, void *vend)
{
  freerange(vstart, vend);
  // TASK 4: initilize total page frames inside the va of the kernel - part 2
  uint virtualStartAddr = PGROUNDUP((uint) vstart);
  uint virtualEndAddr = PGROUNDDOWN((uint) vend);
  totalPgFrameCounter += (virtualEndAddr - virtualStartAddr) / PGSIZE;
  freePgFrameCounter = totalPgFrameCounter;
  kmem.use_lock = 1;
}

void
freerange(void *vstart, void *vend)
{
  char *p;
  p = (char*)PGROUNDUP((uint)vstart);
  for(; p + PGSIZE <= (char*)vend; p += PGSIZE)
    oldKfreeForInit(p);
}


void
oldKfreeForInit(char *v)
{
  struct run *r;

  if((uint)v % PGSIZE || v < end || V2P(v) >= PHYSTOP)
    panic("oldKfreeForInit");

  // Fill with junk to catch dangling refs.
  memset(v, 1, PGSIZE);

  if(kmem.use_lock)
    acquire(&kmem.lock);
  r = (struct run*)v;
  r->next = kmem.freelist;
  kmem.freelist = r;
  if(kmem.use_lock)
    release(&kmem.lock);
}



//PAGEBREAK: 21
// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(char *v)
{
  struct run *r;
  int refsNum;

  if((uint)v % PGSIZE || v < end || V2P(v) >= PHYSTOP)
    panic("kfree");
  
  // TASK 2: decrease ref counter of page
  refsNum = getPageRefs(v);
  if(refsNum == 0){
    panic("cannot free page with 0 refs\n");
  }

  refDecrease(v);
  
  // TASK 2: check that page ref == 0 -> if not, don't free page
  if(getPageRefs(v) == 0){
    // Fill with junk to catch dangling refs.
    memset(v, 1, PGSIZE);

    if(kmem.use_lock)
      acquire(&kmem.lock);
    r = (struct run*)v;
    r->next = kmem.freelist;
    kmem.freelist = r;
    //TASK 4: freeing page frame -> update page frame counter
    freePgFrameCounter++;
    if(kmem.use_lock)
      release(&kmem.lock);
  }
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
char*
kalloc(void)
{
  struct run *r;

  if(kmem.use_lock)
    acquire(&kmem.lock);
  r = kmem.freelist;
  if(r){
    //TASK 4 -> allocating page frame -> updating page frame counter
    freePgFrameCounter--;
    kmem.freelist = r->next;
    //TASK 2: set ref counter to 1
    kmem.refs[(V2P((char*)r)/PGSIZE)] = 1;
  }
  if(kmem.use_lock)
    release(&kmem.lock);
  return (char*)r;
}

//TASK 2: auxiliary funcs

//increase refs of physical page
void refIncrease(char* vAddr){
  if(kmem.use_lock){
    acquire(&kmem.lock);
  }
  kmem.refs[(V2P(vAddr)/PGSIZE)]++;
  if(kmem.use_lock){
    release(&kmem.lock);
  }
}

//decrease refs of physical page
void refDecrease(char* vAddr){
  if(kmem.use_lock){
    acquire(&kmem.lock);
  }
  kmem.refs[(V2P(vAddr)/PGSIZE)]--;
  if(kmem.use_lock){
    release(&kmem.lock);
  }
}

//get ref counter of physical page
int getPageRefs(char *vAddr){
  int res = -1;
  if(kmem.use_lock){
    acquire(&kmem.lock);
  }
  res = kmem.refs[(V2P(vAddr)/PGSIZE)];
  if(kmem.use_lock){
    release(&kmem.lock);
  }
  return res;
}