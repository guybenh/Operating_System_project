#include "types.h"
#include "stat.h"
#include "user.h"

#define PGSIZE 4096
char in[3];
int* pages[18];
int* cowPages[10];

int main(int argc, char *argv[]){
  //COW TEST 1 - father allocates 10 pages with data, child changes all pages data, but father's data remain the same
  printf(1,"starting cow test 1\n");
  for(int i=0; i<10; i++){
    printf(1, "father allocated page %d with data: %d\n", i, i);
    cowPages[i] = (int*)sbrk(PGSIZE);
    *cowPages[i] = i;
  }
  if(fork() == 0){
    for(int i=0; i<10; i++){
      printf(1,"child reading page %d with data: %d\n", i, *cowPages[i]);
      printf(1,"child changing page %d data to be -> data + 1\n");
      *cowPages[i] = i+1;
      printf(1,"page %d new data is now %d\n", i ,*cowPages[i]);
    }
    exit();
  }

  wait();
  //father makes sure his data stayed the same
  for(int i=0; i<10; i++){
    printf(1, "father's data after child died is %d\n", *cowPages[i]);
  }

  sbrk(-10*PGSIZE); // delete previous 10 pages
  printf(1,"---------press enter to continue------------\n");
  gets(in,3);

  //COW TEST 2 - check number of total free pages
  printf(1,"starting cow test 2\n");
  for(int i=0; i<10; i++){
    printf(1, "father allocated page %d with data: %d\n", i, i);
    cowPages[i] = (int*)sbrk(PGSIZE);
    *cowPages[i] = i;
  }
  //make sure number of free pages is lower by 10 (10 pages were allocated above)
  printf(1,"father forking new child\n");

  //father forks, and we make sure the num of total free pages stays the same
  if(fork() == 0){
    printf(1,"child process --- num of total free pages is: %d\n", getNumberOfFreePages());
    for(int i=0; i<10; i++){
      printf(1,"child process reads page num %d with data %d\n", i,*cowPages[i]);
      if(i>4){
        printf(1,"child process writes to page num %d\n", i);
        *cowPages[i] = i+1;
      }
    }
    //num of free pages should be lower by 5 - there were 5 write pagefaults above
    printf(1,"child process --- num of total free pages is: %d\n", getNumberOfFreePages());
    exit();
  }

  wait();
  sbrk(-10*PGSIZE); // delete previous 10 pages
  printf(1,"---------press enter to continue------------\n");
  gets(in,3);

  //TEST 2 - fork and child allocating 28 pages
  printf(1, "--------------------TEST 2:----------------------\n");
  printf(1, "-------------allocating 28 pages-----------------\n");
  if(fork() == 0){
    for(int i = 0; i < 28; i++){
        printf(1, "doing sbrk number %d\n", i);
        sbrk(PGSIZE);
    }
    printf(1, "------------child --> allocated_memory_pages: 16 paged_out: 16------------\n");
    printf(1, "--------for our output press CTRL^P:--------\n");
    printf(1,"---------press enter to continue------------\n");
    gets(in,3);
    exit();
  }
  wait();
    
  //TEST 3 - father wait for child and then allocating 18 pages
  printf(1,"---------press enter to continue------------\n");
  gets(in,3);
  printf(1, "--------------------TEST 3:----------------------\n");
  for(int i = 0; i < 18; i++){
    printf(1, "i: %d\n", i);
    pages[i] = (int*)sbrk(PGSIZE);
    *pages[i] = i;
  }
  printf(1, "--------father --> allocated_memory_pages: 16 paged_out: 6--------\n");
  printf(1, "--------for our output press CTRL^P:--------\n");
  printf(1,"---------press enter to continue------------\n");
  gets(in,3);

  //TEST 4 - fork from father & check if child copy file & RAM data and counters
  printf(1, "--------------------TEST 4:----------------------\n");
  if(fork() == 0){
    for(int i = 0; i < 18; i++){
        printf(1, "expected: %d, our output: %d\n",i,*pages[i]);
    }
    printf(1, "--------------expected: allocated_memory_pages: 16 paged_out: 6--------------\n");
    exit();
  }
  sleep(5);
  wait();
  printf(1, "---------------press enter to continue---------------\n");
  gets(in,3);

  //TEST 4 - deleting RAM
  printf(1, "-----------deleting physical pages-----------\n");
  sbrk(-16*PGSIZE);
  if(fork() == 0){
    printf(1, "--------total pages for process is should be 6--------\n");
    printf(1, "--------for our output press (CTRL^P):--------\n");
    exit();
  }
  wait();
  printf(1, "--------------press enter to continue--------------\n");
  gets(in,3);
  
  // TEST 5 - fail to read pages[17] beacause it deleted from memory
  if(fork() == 0){
    printf(1, "---------------TEST 5 should fail on access to *pages[17]---------------\n");
    printf(1, "%d", *pages[17]);
  }
  wait();
  printf(1, "**************************** All tests passed ****************************\n");
  exit();
}