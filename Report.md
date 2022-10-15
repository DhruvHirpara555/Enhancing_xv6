# Enhancing xv6

xv6 is an elementary OS developed by MIT to demonstrate various concepts of OS. We implemented a few system-calls, Copy-on-Write fork and various scheduling algorithms in xv6.



##  Specification 1 : 

### Part 1 : strace (Tracing syscalls)

1. Added`strace.c` in `user/`, added `$U/_strace` to `UPROGS` in Makefile.

2. Added a variable `mask` in `struct proc` to keep track of what system calls to print by checking the bits in it.

3. Created a `sys_trace()` function in `kernel/sysproc.c`, which is used to assign the passed value by using `argint` to the `tracemask` variable of the currently running process.

4. Added `np->mask = p->mask` to the `fork()` function to pass the `tracemask` value of the parent to the child created.

5. In `kernel/syscall.c`,  added  `sys_trace` function to the array of function pointers `syscalls`.

6. In `kernel/syscall.h`, added macro `SYS_trace` macro.

7. In `user/usys.pl`, added entry for `trace` .

8. the syscall name using the `syscallnames` array, and the arguments in integer form by checking the number of arguments for the syscall using `syscallargs` and the values using the `argint()` function. The return value is stored in `a0 ` after execution of the syscall. In  `user/user.h`, added the definition of the syscall `trace`.

9. Created two more arrays `syscallnames` and `syscallargs` to store the name of each syscall and to store the number of arguments it takes respectively.

10. After execution we check if the syscall executed is to be tracked. If it is, we print all the necessary info, along with the pid using `p->pid`, the syscall name using the `syscallnames` array, and the arguments in integer form by checking the number of arguments for the syscall using `syscallargs` and the values using the `argint()` function. The return value is stored in `a0` after execution of the syscall.

    

### Part 2 : sigalarm and sigreturn 

1. Added variable
   * alarm_ticks:-                 alarm time 
   * current_ticks:-               current time
   * int alarm_flag:-                     alarm flag
   * *struct* trapframe *trapframe_backup; *// backup trapframe*
   *  *void* (*alarm_handler)(*void*);        *// address alarm handler*
2. syscall sigalarm sets alarm ticks to the arg1 ,alarmhandler to arg2 and alarmglag to 1
3.  in trap.c we are saving the curr trapeframe if curr_ticks is greater than alarmticks and changing the curr trapeframes epc to address of alarmhandler
4. In sigreturn we restore the saved trapeframe and backup a0 since sigreturn is syscall it changes the return value.

​    

## Specification 2: Scheduler

* Added support for passing macros when running `make`. If `make qemu SCHEDULER=MLFQ` is executed in the shell, then all the files will be compiled with a `-D MLFQ` flag, hence defining the macro specified in all files.

### FCFS

1. Added `start_ticks` in `struct proc` and allocated  its value to ticks in `allocproc()`.
2. Disabled timer interrupt by removing the line(`yeild()`) which after the check if `which_dev == 2`(timer interrupt)   in `usertrap()` and `kerneltrap()` in the `kernel/trap.c` file.
3. A function `fcfs_scheduler()` that is called by `scheduler()` is defined. It goes through all the `RUNNABLE` processes in the process array, and selects the process that has the lowest `start_ticks` value (created the earliest), and then runs the process.

### LBS

1. Added `tickets` in `struct proc` and allocated  its value to 1 (default value).
2. Added line `np->tickets = p ->tickets` in fork to inherit the parents tickets
3. Added a new syscall `settickets` to set the tickets of the calling process
4. Enable timer interrupt by calling `yield()` under the check if `which_dev == 2`(timer interrupt)   in `usertrap()` and `kerneltrap()` in the `kernel/trap.c` file.'
5. Created Pseudo Random generator function `rand()` to generate random no.
6. A function `lottery_scheduler()` that is called by `scheduler()` is defined. It goes through all the `RUNNABLE` processes in the process array, and calculate `totaltickets`
7. Then it generates a random no. between 0 and totaltickets
8. Then it goes through while adding the tickets of processes in `curr` and selects the process as soon as `curr` surpasses the random number generated and then runs the process.

### PBS

1. Added variables

   * `run_ticks`: - run time from last time it was schedule initialized to 0 in `allocproc()`
   * `sleep_ticks`: - sleep time from last time it was schedule initialized to 0 in `allocproc()`
   *  `num_scheduled`:- number of times it was scheduled initialized to 0 in `allocproc()`
   * `static_priority`:- static priority initialized to 60 in `allocproc()`

   in the stuct proc.

2. Implemented `dynamic_priority(struct proc *p)` which calculates dynamic priority from niceness and static priority. 

3. Disabled timer interrupt by removing the line(`yeild()`) which after the check if `which_dev == 2`(timer interrupt)   in `usertrap()` and `kerneltrap()` in the `kernel/trap.c` file.

4. Added syscall setpriority which can change the static priority. And also yields if the new dynamic priority value is lower than old dynamic priority value and sets `run_ticks` and `sleep_ticks` to 0 which will result in niceness changing to 5

5. A function `priority_scheduler()` that is called by `scheduler()` is defined. It goes through all the `RUNNABLE` processes in the process array, and selects the process that has the lowest `dynamic_priority()` value which means highest priority. In case of tie  compare the number of times scheduled `num_scheduled` (lower is better), if again equal then compare the time it was `start_ticks` (lower is better)

### MLFQ

1. Implemented struct queue data structure which contains `struct proc *` array of size `NPROCS`and int head, int tail, int size.

2. Added `queueinit()` which initializes 5 queues.

3. Added variables

   * `curr_q`:- Current queue of the process  initialized to 0 in `allocproc()`
   * `q_ticks[5]`:- Ticks in each queue  initialized to  0 for every  in `allocproc()`
   * `cq_rticks`:- RunTicks in current queue after last switch  initialized to 0 in `allocproc()`
   * `q_enter_time`:- Time when the process entered the queue  initialized to ticks in `allocproc()`
   * `qued_fl`:- Flag to check if the process is qued  pushed in mlfqs[0] initialized to 1 in `allocproc()`

   in the stuct proc.

4. Edited `kerneltrap()` and `usertrap()` to yield when process has exhausted its time slice and also yields if higher priority queue has process(checks every timer interrupt).

5. Implemented aging function `queue_switch` which is called every clock interrupt to check the aging and age the process.

6. A function `mlfq_scheduler()` that is called by `scheduler()` is defined.  which schedules the process according to MLFQ policy.

#### Possible exploitation of implemented scheduling algorithm

In accordance with the instructions, a process that willingly gives up CPU control before the end of its time slice returns to the same queue rather than being given a lower priority.

While this may seem reasonable and should work most of the time, some people may be aware of the design of this policy and try to take advantage of it so that their processes receive a higher priority than others, receiving an unfair amount of processing power.

A process can be designed so that, just before the time slice is about to expire, it can make a very short I/O request to release itself from the CPU and return to the same priority level very fast if the user understands how long the time slice is in each priority level. As a result, it succeeds in avoiding the reduction in priority level. As a result, the process might remain at the same priority level and continue to occupy the majority of the processor's time.

## Specification 3 : 

### Copy on Write Fork

#### Change to be Made

Since xv6 copies the memory content of the parent process into the child process, this results in inefficient usage of memory since the child may only read from memory. We need to make it such that when `fork` is called, both parent and child process share the same physical memory, after marking it read-only.

When any of the processes tries to modify the shared memory, a page-fault is generated, which is then caught and handled to create a copy of the physical memory into a new address and assign it to the calling  process, whilst giving it the required write permissions. This ensures that we use main memory more efficiently.

#### Implementation

1. Implemented a data structure `n` to keep track of the number of processes referring to a physical address page. 
2. Also added a custom flag `PTE_COW`. If set, it means that that virtual address mapped by that PTE refers to a shared physical memory (due to Copy on Write).
3. Modified `uvmcopy()` in `vm.c`, which is called in `fork()` so that it increments the `num_ref_to_page` (after acquiring the lock) value for the physical address.
4. The permission flags for both the parent and child are changed to make them read-only (using the `PTE_W` flag). The `PTE_COW` flag is also set for both the processes. `sfence_vma()` is used to flush and refresh the flags of the processes.
5. Using `mapppages()`, the virtual addresses assigned to the child process are also mapped to the same physical address. 
6. Modified `kalloc()` in `kalloc.c` to set the `num_ref_to_page` for the concerned physical memory to 1.
7. Modified `kfree()`in the same file, to make it such that it just decrements the `num_ref_to_page` to that physical address when there are more than 1 processes using the specified physical memory. It frees the physical memory only when the number of references to it becomes 0.
8. When a process tries to write into the shared physical memory, it causes a pagefault with `scause` 15 to occur. It is caught in the `usertrap()` function in `trap.c` by checking for `r_scause == 15` and handled using custom `cowfault()`.
9. When `cowfault()` is called, it first checks if the relevant PTE's `PTE_COW` bit is set. If the bit is set, then it allocates a new physical space to the process causing the pagefault using `kalloc()` and assigns it write permission (`PTE_COW` for this is not set).
10. It then uses `memmove` to copy the contents of the physical address page to the new location also. It finally calls `kfree()` on the original physical page.
11. `cowfault()` is also called in the `copyout()` function to handle the case when the `dstva` points to a read-only memory set by CoW.



## Performance Analysis


- **Benchmarking**

  |        Scheduler         | <rtime> | <wtime> |
  | :----------------------: | :-----: | :-----: |
  |       Round robin        |   16    |   166   |
  |  First come first serve  |   16    |   133   |
  | Lottery based scheduler  |   16    |   169   |
  | Priority Based Scheduler |   16    |   100   |
  |             MLFQ Scheduler             |  16  |     160   |

The above results are obtained by running `schedulertest` on a single CPU.



### Scheduling Graph of MLFQ

<img src="/home/dhruv/OSNftw/Enhancing_xv6/Enhancing_xv6/output.png" alt="output" style="zoom:150%;" />
