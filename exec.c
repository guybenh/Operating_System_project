#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "defs.h"
#include "x86.h"
#include "elf.h"

int
exec(char *path, char **argv) 
{
  char *s, *last;
  int i, off;
  uint argc, sz, sp, ustack[3+MAXARG+1];
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pde_t *pgdir, *oldpgdir;
  struct proc *curproc = myproc();

  //backing up process data
  int physCount = curproc->physCounter;
  int swapCount = curproc->fileCounter;
  struct memPage savePages[MAX_TOTAL_PAGES];
  struct memPage * physHeadTmp = curproc->physHead;

  begin_op();

  if((ip = namei(path)) == 0){
    end_op();
    cprintf("exec: fail\n");
    return -1;
  }
  ilock(ip);
  pgdir = 0;

  // Check ELF header
  if(readi(ip, (char*)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((pgdir = setupkvm()) == 0)
    goto bad;

  if(SELECTION != NONE){
    for (int i = 0; i < MAX_TOTAL_PAGES; i++){
      savePages[i].pageData = curproc->allPages[i].pageData;
      if (curproc->allPages[i].prev != 0){
        savePages[i].prev = curproc->allPages[i].prev;
      }
      if (curproc->allPages[i].next != 0){
        savePages[i].next = curproc->allPages[i].next;
      }
    }
    physHeadTmp = curproc->physHead;
    //init new process data
     for (int i = 0; i < MAX_TOTAL_PAGES; i++){
      curproc->allPages[i].pageData.va = 0xFFFFFFFF;
      curproc->allPages[i].pageData.state = FREE;
      curproc->allPages[i].pageData.offsetIndex = -1;
      curproc->allPages[i].pageData.indexInAllPages = i;
      curproc->allPages[i].prev = 0;
      curproc->allPages[i].next = 0;
    //TASK 3//
      curproc->allPages[i].pageData.ageCounter = SELECTION == LAPA ? 0xFFFFFFFF : 0;
    /////////
    }
    //TASK 4
    curproc->pageFaults = 0;
    curproc->pageTotalNumberOfPagedOut = 0;
    //
    memset(&curproc->freeOffsetInFile,0,17);
    curproc->swapFile = 0;
    curproc->physHead = 0;
    curproc->physCounter = 0;
    curproc->fileCounter = 0;
  }

  // Load program into memory.
  sz = 0;
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, (char*)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if((sz = allocuvm(pgdir, sz, ph.vaddr + ph.memsz)) == 0)
      goto bad;
    if(ph.vaddr % PGSIZE != 0)
      goto bad;
    if(loaduvm(pgdir, (char*)ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  // Allocate two pages at the next page boundary.
  // Make the first inaccessible.  Use the second as the user stack.
  sz = PGROUNDUP(sz);
  if((sz = allocuvm(pgdir, sz, sz + 2*PGSIZE)) == 0)
    goto bad;
  clearpteu(pgdir, (char*)(sz - 2*PGSIZE));
  sp = sz;

  // Push argument strings, prepare rest of stack in ustack.
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp = (sp - (strlen(argv[argc]) + 1)) & ~3;
    if(copyout(pgdir, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[3+argc] = sp;
  }
  ustack[3+argc] = 0;

  ustack[0] = 0xffffffff;  // fake return PC
  ustack[1] = argc;
  ustack[2] = sp - (argc+1)*4;  // argv pointer

  sp -= (3+argc+1) * 4;
  if(copyout(pgdir, sp, ustack, (3+argc+1)*4) < 0)
    goto bad;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(curproc->name, last, sizeof(curproc->name));

  // Commit to the user image.
  oldpgdir = curproc->pgdir;
  curproc->pgdir = pgdir;
  curproc->sz = sz;
  curproc->tf->eip = elf.entry;  // main
  curproc->tf->esp = sp;

  if(SELECTION != NONE){
    //create new swapfile
    if(curproc->swapFile != 0){
      removeSwapFile(curproc);
      createSwapFile(curproc);
    }
  }
  switchuvm(curproc);
  freevm(oldpgdir);

  return 0;

 bad:
  if(pgdir)
    freevm(pgdir);
  if(ip){
    iunlockput(ip);
    end_op();
  }
  if(SELECTION != NONE){
    curproc->physCounter = physCount;
    curproc->fileCounter = swapCount;
    curproc->physHead = physHeadTmp;

    //restoring process data in case of failure
    for (int i = 0; i < MAX_TOTAL_PAGES; i++){
      curproc->allPages[i].pageData = savePages[i].pageData;
      if (savePages[i].prev != 0){
        curproc->allPages[i].prev = savePages[i].prev;
      }
      if (savePages[i].next != 0){
        curproc->allPages[i].next = savePages[i].next;
      }
    }
  }
  return -1;
}
