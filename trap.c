#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;
int pageSwap(uint va);
pte_t* walkpgdirImport(pde_t *pgdir, const void *va, int alloc);
int mappagesImport(pde_t *pgdir, void *va, uint size, uint pa, int perm);

void cowPgFault(uint va, pte_t* pte){
  uint pa = PTE_ADDR(*pte);
  uint flags = PTE_FLAGS(*pte) | PTE_W;
  int refCount = getPageRefs((char*)va);
  if(refCount == 1){
    *pte = *pte | PTE_W;
    lcr3(V2P(myproc()->pgdir));
  }
  else if(refCount > 1){  // create new writeable copy
    char* newVAddr = kalloc();
    if(newVAddr == 0){
      myproc()->killed = 1;
    }
    else{
      memmove(newVAddr,(char*)P2V(pa),PGSIZE);  //copy page contents
      *pte = V2P(newVAddr) | flags;
      lcr3(V2P(myproc()->pgdir));
      refDecrease((char*)va);
    }
  }
  else{
    panic("should not happen, ref count is 0");
  }
}


void displayLst(){
    struct memPage* check = myproc()->physHead;
    while(check!=0){
      pte_t* pte = walkpgdirImport(myproc()->pgdir, (char*)check->pageData.va, 0);
      cprintf("(%x ,%d)-->",check->pageData.va, (*pte & PTE_A));
      check = check->next;
    }
    cprintf("\n");
}


void pageAlgoAux(pte_t* pte){
  //update age counter of page acceded - NFU || LAPA
  if(SELECTION == (NFUA || LAPA) && (myproc()->pid > 2)){
    struct memPage* RAMList = myproc()->physHead;
    while (RAMList != 0){
      RAMList->pageData.ageCounter >>= 1;
      pte = walkpgdirImport(myproc()->pgdir, (char*)RAMList->pageData.va, 0);
      if(*pte & PTE_A){
        //adding 1 bit for acceded page to the MSB
        RAMList->pageData.ageCounter |= 0x80000000;  //2^31
        *pte &= (~PTE_A); //turning off the bit
      }
      RAMList = RAMList->next;
    }
  }
  else if((SELECTION == AQ) && (myproc()->pid > 2)){

    // displayLst();

    //update queue
    struct memPage* RAMList = myproc()->physHead;
    while ((RAMList != 0) && (RAMList->next != 0)){
      pte_t* curPte = walkpgdirImport(myproc()->pgdir, (char*)RAMList->pageData.va, 0);
      pte_t* nextPte = walkpgdirImport(myproc()->pgdir, (char*)RAMList->next->pageData.va, 0);
      //cur and next links are present -> turn off cur bit
      if((*curPte & PTE_A) && (*nextPte & PTE_A)){
        *curPte &= (~PTE_A);
      }
      //cur=1 & next=0 -> switch links & and turn off cur bit
      else if((*curPte & PTE_A) && (!(*nextPte & PTE_A))){
        // cprintf("got inside AQ updates\n");
        //e.g. 0<--A<-->B<-->C-->0   ----->   0<--B<-->A<-->C-->0
        struct memPage* prev = RAMList->prev;
        struct memPage* A = RAMList;
        struct memPage* B = RAMList->next;
        struct memPage* C = RAMList->next->next;
        //update head if A->prev == 0  ---> H = B
        if(prev == 0){
          myproc()->physHead = B;
        }
        else{
          prev->next = B;
        }
        B->prev = prev;
        B->next = A;
        A->prev = B;
        A->next = C;
        if(C != 0){
          C->prev = A;
        }
        //turn off A bit
        *curPte &= (~PTE_A);
      }
      RAMList = RAMList->next;
    }
    //last link PTE_A is presented
    if(RAMList != 0){
      pte_t* edgeCase = walkpgdirImport(myproc()->pgdir, (char*)RAMList->pageData.va, 0);
      if(*edgeCase & PTE_A){
        *edgeCase &= (~PTE_A);
      }
    }
  }
}

void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  if(tf->trapno == T_SYSCALL){
    if(myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if(myproc()->killed)
      exit();
    return;
  }

  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    if(cpuid() == 0){
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;
  case T_PGFLT:
    //increment number of page faults
    myproc()->pageFaults++;
    // use CR2 register to determine the faulting address and identify the page.
    uint va = PGROUNDDOWN(rcr2()); 
    pte_t* pte = walkpgdirImport(myproc()->pgdir, (char*)va, 0);
    //p is init or shell or SELECTION is NONE -> not swapping
    if(!(*pte & PTE_P) && (myproc()->pid <= 2 || SELECTION == NONE)) { 
      goto defaultLabel;
    }
    else {
      //updating LAPA | NFUA | AQ
      pageAlgoAux(pte);
    }
    if(*pte & PTE_P){   // if PTE_P is set, then the page fault is a write page fault -> COW case handler
      if(*pte & PTE_COW){ //for readonly original pages
        *pte = *pte | PTE_W;
        *pte = *pte & ~PTE_COW;
      }
      cowPgFault(va, pte);
    }
    else if(SELECTION != NONE){      //pgfault not related to cow
      // cprintf("inside T_PGFLT trap.c \n");
      pte_t* pte = walkpgdirImport(myproc()->pgdir, (char*)va, 0);
      if(((*pte & PTE_PG) == 0)){ // page not found in memory or swapfile
        cprintf("going to seg-fault with PTE_P %d\n", *pte & PTE_P);
        goto defaultLabel;
      }
      else{
        //because of the page fault(ram is full), need to swap with page from file

        if(pageSwap(va) < 0){
          panic("swappage failed in trap.c\n");
        }
      }
    } else { //in case page algorithm -> NONE
      goto defaultLabel;
    }
    break;

  //PAGEBREAK: 13
  default:
    defaultLabel: // page fault - page not exist
    cprintf("process %d is inside default case\n", myproc()->pid);
    if(myproc() == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(myproc() && myproc()->state == RUNNING &&
     tf->trapno == T_IRQ0+IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();
}
