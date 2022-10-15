#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];
extern struct que mlfqs[MLFQ_LEVELS];


struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}

void
mlfq_init(void)
{
    for(int i = 0; i < MLFQ_LEVELS; i++){
        mlfqs[i].head = 0;
        mlfqs[i].tail = 0;
        mlfqs[i].size = 0;
    }
}

// }
// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;
  p->alarm_flag = 0;
  p->alarm_ticks = 0;
  p->current_ticks = 0;
  p->alarm_handler = 0;
  p->tickets = 1;
  p->sleep_ticks = 0;
  p->run_ticks = 0;
  p->ready_ticks = 0;
  p->static_priority = 60;
  p->num_scheduled = 0;
  // acquire(&tickslock);
  p->start_ticks = ticks;
  // release(&tickslock);
  p->rtime = 0;
  p->etime = 0;
  p->ctime = ticks;

  p->curr_q = 0;
  for(int i = 0; i < MLFQ_LEVELS; i++){
      p->q_ticks[i] = 0;
  }
  p->q_enter_time = ticks;
  que_push(&mlfqs[0], p);
  // printf("%d\n",mlfqs[0]->size);
  p->qued_fl = 1;
  p->cq_rticks = 0;



  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    // decrease_num_ref((uint64)(p->trapframe));
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  if(p->trapframe_backup)
    kfree((void*)p->trapframe_backup);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
  p->alarm_flag = 0;
  p->alarm_ticks = 0;
  p->current_ticks = 0;
  p->alarm_handler = 0;
  p->start_ticks = 0;
  p->tickets = 0;
  p->sleep_ticks = 0;
  p->run_ticks = 0;
  p->ready_ticks = 0;
  p->static_priority = 0;
  p->num_scheduled = 0;


  p->curr_q = 0;
  p->qued_fl = 0;
  for(int i = 0; i < MLFQ_LEVELS; i++){
      p->q_ticks[i] = 0;
  }
  p->q_enter_time = 0;
  p->cq_rticks = 0;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;
  np->mask = p->mask;
  np->tickets = p->tickets;
  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);
  p->xstate = status;
  p->state = ZOMBIE;
  p->etime = ticks;


  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}
int
waitx(uint64 addr, uint* wtime, uint* rtime)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          *rtime = np->rtime;
          *wtime = np->etime - np->ctime - np->rtime;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// void
// update_time()
// {
//   struct proc* p;
//   for (p = proc; p < &proc[NPROC]; p++) {
//     acquire(&p->lock);
//     if (p->state == RUNNING) {
//       p->rtime++;
//     }
//     release(&p->lock);
//   }
// }

void update_ticks(void){
  struct proc *p;
  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->state == RUNNING){
      p->run_ticks++;
      p->rtime++;

      p->cq_rticks++;
      p->q_ticks[p->curr_q]++;

    }
    else if(p->state == SLEEPING){
      p->sleep_ticks++;
    }
    else if(p->state == RUNNABLE){
      p->ready_ticks++;
    }
    release(&p->lock);
    // if(p->pid != 0 && p->pid != 1 && p->pid != 2){
    //   printf("%d %d %d\n", p->pid, p->curr_q, ticks);
    // }
  }


  queue_switch();
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  // struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();
    #ifdef LBS
    lottery_scheduler(c);
    #endif
    #ifdef RR
    round_robin_scheduler(c);
    #endif
    #ifdef FCFS
    fcfs_scheduler(c);
    #endif
    #ifdef PBS
    priority_scheduler(c);
    #endif
    #ifdef MLFQ
    mlfq_scheduler(c);
    #endif
    // for(p = proc; p < &proc[NPROC]; p++) {
    //   acquire(&p->lock);
    //   if(p->state == RUNNABLE) {
    //     // Switch to chosen process.  It is the process's job
    //     // to release its lock and then reacquire it
    //     // before jumping back to us.
    //     p->state = RUNNING;
    //     c->proc = p;
        // swtch(&c->context, &p->context);

    //     // Process is done running for now.
    //     // It should have changed its p->state before coming back.
    //     c->proc = 0;
    //   }
    //   release(&p->lock);
    // }
  }
}

void
fcfs_scheduler(struct cpu *c)
{
  struct proc *p;
  struct proc *min_proc = 0;
  uint64 min_sticks = -1;
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == RUNNABLE) {
      if(min_sticks == -1) {
        min_sticks = p->start_ticks;
        min_proc = p;
        // continue;
      }
      if(p->start_ticks < min_sticks) {
        release(&min_proc->lock);
        min_proc = p;
        min_sticks = p->start_ticks;
        // continue;
      }
    }
    if(min_proc!= p){
      release(&p->lock);
    }

  }
  if(min_sticks != -1 ){
    // acquire(&min_proc->lock);
    if(min_proc->state != RUNNABLE) {
      release(&min_proc->lock);
      return;
    }
    min_proc->state = RUNNING;
    c->proc = min_proc;
    swtch(&c->context, &min_proc->context);
    c->proc = 0;
    release(&min_proc->lock);
  }
}

void round_robin_scheduler(struct cpu *c)
{
  struct proc *p;
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == RUNNABLE) {
      // Switch to chosen process.  It is the process's job
      // to release its lock and then reacquire it
      // before jumping back to us.
      p->state = RUNNING;
      c->proc = p;
      swtch(&c->context, &p->context);

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&p->lock);
  }
}
//implent rand
int rand(void)
{
  static unsigned long int next = 1;
  next = next * 1103515245 + ticks;
  return((unsigned)(next/65536) % 32768);
}

void lottery_scheduler(struct cpu *c)
{
  struct proc *p;
  int total_tickets = 0;
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == RUNNABLE) {
      total_tickets += p->tickets;
    }

    // release(&p->lock);
  }
  if(total_tickets == 0) {
    for(p = proc; p < &proc[NPROC]; p++) {
      release(&p->lock);
    }
    return;
  }
  int tochoose = rand() % total_tickets;
  int curr = 0;
  int flag = 0;
  struct proc* win = 0;
  for(p = proc; p < &proc[NPROC]; p++) {
    // acquire(&p->lock);
    if(p->state == RUNNABLE) {
      curr += p->tickets;
      if(curr >= tochoose && flag == 0) {
        // p->state = RUNNING;
        // c->proc = p;
        // swtch(&c->context, &p->context);
        // c->proc = 0;
        // release(&p->lock);
        // return;
        win = p;
        flag = 1;
        continue;

      }
    }
    release(&p->lock);
  }

  if(win != 0 ) {
    // acquire(&win->lock);
    if(win->state != RUNNABLE) {
      release(&win->lock);
      return;
    }
    win->state = RUNNING;
    c->proc = win;
    swtch(&c->context, &win->context);
    c->proc = 0;
    release(&win->lock);
  }
}

int dynamic_priority(struct proc *p)
{
  int niceness;
  if(((p->sleep_ticks + p->run_ticks) == 0)){
    niceness = 5;
  }
  else{
    niceness = p->sleep_ticks/(p->sleep_ticks + p->run_ticks);
    niceness = niceness * 10;
  }

  int DP = p->static_priority - niceness + 5;
  if(DP > 100){
    DP = 100;
  }
  if(DP < 0){
    DP = 0;
  }
  return DP;
}

void
priority_scheduler(struct cpu *c)
{
  struct proc *p;
  struct proc *high_proc = 0;
  // int high_priority = 101;
  for (p = proc; p < &proc[NPROC]; p++){
    // int flag = 0;
    acquire(&p->lock);
    if(p->state == RUNNABLE){

      // comparing .............
      if(high_proc == 0){
        high_proc = p;
        // flag = 1;
      }
      else{
        int high_priority = dynamic_priority(high_proc);
        int curr_priority = dynamic_priority(p);
        if(curr_priority < high_priority){
          release(&high_proc->lock);
          high_proc = p;
          // flag = 1;
        }
        else if (curr_priority == high_priority) {
          if(p->num_scheduled < high_proc->num_scheduled){
            release(&high_proc->lock);
            high_proc = p;
            // flag = 1;
          }
          else if(p->num_scheduled == high_proc->num_scheduled){

            if(p->start_ticks < high_proc->start_ticks){
              release(&high_proc->lock);
              high_proc = p;
              // flag = 1;
            }
          }
        }
      }



    }
    if(p != high_proc){
      release(&p->lock);
    }

  }

  // scedueling
  if(high_proc != 0){
    // acquire(&high_proc->lock);
    if(high_proc->state == RUNNABLE){
      high_proc->state = RUNNING;

      high_proc->num_scheduled++;
      high_proc->run_ticks = 0;
      high_proc->sleep_ticks = 0;

      c->proc = high_proc;
      swtch(&c->context, &high_proc->context);
      c->proc = 0;
    }
    release(&high_proc->lock);
  }


}

void queue_switch()
{


  for(int q=1; q<MLFQ_LEVELS; q++){

    while(!que_empty(&mlfqs[q])){
      struct proc *p = que_front(&mlfqs[q]);
      acquire(&p->lock);
      if(p->state == RUNNABLE && (ticks - p->q_enter_time) > 30){
        que_pop(&mlfqs[q]);
        p->curr_q--;
        // p->q_enter_time = ticks;
        p->cq_rticks = 0;
        que_push(&mlfqs[q-1], p);
        release(&p->lock);
      }
      else{
        release(&p->lock);
        break;
      }

    }
  }

}


void mlfq_scheduler(struct cpu *c)
{
  // switch queue if any process is suppose to change ques
  queue_switch();
  // que all unqueued processes to queue their curr_q
  struct proc *p;
  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == RUNNABLE && p->qued_fl == 0) {
      p->q_enter_time = ticks;
      p->cq_rticks = 0;
      p->qued_fl = 1;
      que_push(&mlfqs[p->curr_q], p);
    }
    release(&p->lock);
  }
  struct proc *torun = 0;
  for (int q = 0; q < MLFQ_LEVELS; q++) {
    while(!que_empty(&mlfqs[q])) {
      struct proc *p = que_pop(&mlfqs[q]);
      acquire(&p->lock);
      p->qued_fl = 0;
      if(p->state == RUNNABLE) {
        torun = p;
        break;
      }

      release(&p->lock);
    }
    if(torun != 0) {
      break;
    }
  }
  if(torun != 0) {
    torun->state = RUNNING;
    c->proc = torun;
    swtch(&c->context, &torun->context);
    c->proc = 0;
    torun->q_enter_time = ticks;
    torun->cq_rticks = 0;
    torun->qued_fl = 1;
    // if(torun->cq_rticks >= (1 << torun->curr_q)) {
    //   if(torun->curr_q < MLFQ_LEVELS - 1) {
    //     torun->curr_q++;
    //   }
    //   torun->cq_rticks = 0;
    //   torun->qued_fl = 1;
    //   que_push(&mlfqs[torun->curr_q], torun);
    // }
    // else {
    //   torun->qued_fl = 1;
    //   que_pushfront(&mlfqs[torun->curr_q], torun);
    // }
    que_push(&mlfqs[torun->curr_q], torun);

    release(&torun->lock);
  }
}



// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;

  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s %d %d %d %d %d %d %d", p->pid, state, p->name, p->q_ticks[0], p->q_ticks[1], p->q_ticks[2], p->q_ticks[3], p->q_ticks[4], p->tickets, p->static_priority);
    // printf("%d %d %d %d %d",mlfqs[0]->head,mlfqs[1]->head,mlfqs[2]->head,mlfqs[3]->head,mlfqs[4]->head);
    printf("\n");
  }
}

void
trace(uint mask){
  struct proc *p = myproc();
  // acquire(&p->lock);
  p->mask = mask;
  // release(&p->lock);
}

void
sigalarm(uint64 ticks, void (*handler)(void)){
  struct proc *p = myproc();
  p->alarm_ticks = ticks;
  p->alarm_handler = handler;
  p->alarm_flag = 1;
  // p->trapframe->a0 = p->a0_backup;


}

void
sigreturn(){
  struct proc *p = myproc();


  // restore the registers
  p->trapframe_backup->kernel_hartid = p->trapframe->kernel_hartid;
  p->trapframe_backup->kernel_satp = p->trapframe->kernel_satp;
  p->trapframe_backup->kernel_trap = p->trapframe->kernel_trap;
  p->trapframe_backup->kernel_sp = p->trapframe->kernel_sp;
  // p->trapframe_backup->a0 = p->trapframe->a0;
  memmove(p->trapframe, p->trapframe_backup, sizeof(struct trapframe));



  // free the backup
  kfree(p->trapframe_backup);
  p->trapframe_backup = 0;
  p->alarm_flag = 1;
  p->current_ticks = 0;

  if(p->alarm_ticks ==0 && p->alarm_handler == 0){

    p->alarm_flag =0;
  }
  p->a0_backup = p->trapframe->a0;
  return;
  // return;


}

void
settickets(int number)
{
  struct proc *p = myproc();
  p->tickets = number;
}

int
set_priority(int priority, int pid)
{
  struct proc *p;
  int old_priority = -1;
  for(p = proc; p < &proc[NPROC]; p++){
    int flag = 0;
    acquire(&p->lock);
    if(p->pid == pid){
      old_priority = p->static_priority;
      p->static_priority = priority;
      p->run_ticks = 0;
      p->sleep_ticks = 0;
      if(p->static_priority < old_priority){
        flag = 1;
      }

      release(&p->lock);
      if(flag == 1){
        yield();
      }
      return old_priority;
    }
    release(&p->lock);
  }
  return old_priority;
}




// queue functions using ll
// struct que
// *que_init(void){
//     struct que *que = (struct que *)kalloc();
//     que->head = 0;
//     que->tail = 0;
//     que->size = 0;
//     return que;
// }

// void
// que_push(struct que *que, struct proc *proc){
//     struct que_node *node = (struct que_node *)kalloc();
//     // printf("pushing in  %d\n",proc->curr_q);
//     node->proc = proc;
//     node->next = 0;
//     if(que->head == 0){
//         que->head = node;
//         que->tail = node;
//     }else{
//         que->tail->next = node;
//         que->tail = node;
//     }
//     que->size++;
// }

// struct proc
// *que_pop(struct que *que){
//     if(que->head == 0){
//         return 0;
//     }
//     struct que_node *node = que->head;
//     struct proc *proc = node->proc;
//     que->head = node->next;
//     if(que->head == 0){
//         que->tail = 0;
//     }
//     kfree((void *)node);
//     que->size--;
//     return proc;
// }

// struct proc
// *que_front(struct que *que){
//     if(que->head == 0){
//         return 0;
//     }
//     return que->head->proc;
// }

// int
// que_empty(struct que *que){
//     return que->head == 0;
// }

// void
// que_pushfront(struct que *que, struct proc *proc){
//     struct que_node *node = (struct que_node *)kalloc();
//     // printf("pushfront in %d\n", proc->curr_q);
//     node->proc = proc;
//     node->next = que->head;
//     que->head = node;
//     if(que->tail == 0){
//         que->tail = node;
//     }
//     que->size++;
// }

// void
// que_remove(struct que *que, struct proc *proc){
//     struct que_node *node = que->head;
//     struct que_node *prev = 0;
//     while(node != 0){
//         if(node->proc == proc){
//             if(prev == 0){
//                 que->head = node->next;
//             }else{
//                 prev->next = node->next;
//             }
//             if(node->next == 0){
//                 que->tail = prev;
//             }
//             kfree((void *)node);
//             que->size--;
//             return;
//         }
//         prev = node;
//         node = node->next;
//     }
//     que->size--;
// }



// queue functions using array
struct que
*que_init(void){
    struct que *que = (struct que *)kalloc();
    que->head = 0;
    que->tail = 0;
    que->size = 0;
    return que;
}

void
que_push(struct que *que, struct proc *proc){
    // printf("pushing in  %d\n",proc->curr_q);
    if(que->size == 0){
        que->head = 0;
        que->tail = 0;
    }
    que->procs[que->tail] = proc;
    que->tail = (que->tail + 1) % NPROC;
    que->size++;
}

struct proc
*que_pop(struct que *que){
    if(que->size == 0){
        return 0;
    }
    struct proc *proc = que->procs[que->head];
    que->head = (que->head + 1) % NPROC;
    que->size--;
    return proc;
}

struct proc
*que_front(struct que *que){
    if(que->size == 0){
        return 0;
    }
    return que->procs[que->head];
}

int
que_empty(struct que *que){
    return que->size == 0;
}

void
que_pushfront(struct que *que, struct proc *proc){
    // printf("pushfront in %d\n", proc->curr_q);
    if(que->size == 0){
        que->head = 0;
        que->tail = 0;
    }
    que->head = (que->head - 1 + NPROC) % NPROC;
    que->procs[que->head] = proc;
    que->size++;
}

