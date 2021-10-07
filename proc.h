#define MAX_PSYC_PAGES 16 //max pages in the physical memory
#define MAX_TOTAL_PAGES 32 //max total pages

// tmp -> to move to another file ??
//SELECTIONS
#define NFUA 1
#define LAPA 2
#define SCFIFO 3
#define AQ 4
#define NONE 5
#define AA 6 //for debug

//TASK 4
//VERBOSE_PRINT
#define FALSE 0
#define TRUE 1



// Per-CPU state
struct cpu {
  uchar apicid;                // Local APIC ID
  struct context *scheduler;   // swtch() here to enter scheduler
  struct taskstate ts;         // Used by x86 to find stack for interrupt
  struct segdesc gdt[NSEGS];   // x86 global descriptor table
  volatile uint started;       // Has the CPU started?
  int ncli;                    // Depth of pushcli nesting.
  int intena;                  // Were interrupts enabled before pushcli?
  struct proc *proc;           // The process running on this cpu or null
};

extern struct cpu cpus[NCPU];
extern int ncpu;

//PAGEBREAK: 17
// Saved registers for kernel context switches.
// Don't need to save all the segment registers (%cs, etc),
// because they are constant across kernel contexts.
// Don't need to save %eax, %ecx, %edx, because the
// x86 convention is that the caller has saved them.
// Contexts are stored at the bottom of the stack they
// describe; the stack pointer is the address of the context.
// The layout of the context matches the layout of the stack in swtch.S
// at the "Switch stacks" comment. Switch doesn't save eip explicitly,
// but it is on the stack and allocproc() manipulates it.
struct context {
  uint edi;
  uint esi;
  uint ebx;
  uint ebp;
  uint eip;
};

enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

////**TASK 1**/////
enum pageState {FREE, FILE, PHYSICAL};

struct page {
  uint va;
  enum pageState state;
  uint offsetIndex;
  uint indexInAllPages; 
  uint ageCounter;   //TASK 3
};

//list of pages in the phys-mem
struct memPage{
    struct page pageData;
    struct memPage* prev;    
    struct memPage* next;    
};
///////////////////

// Per-process state
struct proc {
  uint sz;                     // Size of process memory (bytes)
  pde_t* pgdir;                // Page table
  char *kstack;                // Bottom of kernel stack for this process
  enum procstate state;        // Process state
  int pid;                     // Process ID
  struct proc *parent;         // Parent process
  struct trapframe *tf;        // Trap frame for current syscall
  struct context *context;     // swtch() here to run process
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
  //Swap file. must initiate with create swap file
  struct file *swapFile;      //page file
  // added in task 1
  struct memPage allPages[MAX_TOTAL_PAGES];
  char freeOffsetInFile[17];  // which entery in file is free (by offset) - 17 enteries
  int physCounter;   // counts pages in RAM
  int fileCounter;   // count pages in Disk
  struct memPage *physHead; //head of the pages in the physical memory
  //TASK 4
  int pageFaults; //umber of times the process had page faults 
  int pageTotalNumberOfPagedOut; //total number of times in which pages were paged out
};

// Process memory is laid out contiguously, low addresses first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
