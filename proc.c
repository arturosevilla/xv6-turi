#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

#define INIT_PRIORITY 0

typedef struct process_queue_ {
    struct spinlock lock;
    struct proc proc[NPROC];
} process_queue;

process_queue ptable0, ptable1;


static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan, process_queue *);

void
pinit(void)
{
  initlock(&ptable0.lock, "ptable");
  initlock(&ptable1.lock, "ptable1");
}


static process_queue *get_ptable(int priority)
{
    switch(priority) {
        case 0:
            return &ptable0;
        case 1:
            return &ptable1;
        default:
            panic("incorrect priority when requesting process table\n");
            break;
    }
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(int priority)
{
  struct proc *p;
  char *sp;
  process_queue *ptable = get_ptable(priority);


  acquire(&ptable->lock);
  for(p = ptable->proc; p < &ptable->proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;
  release(&ptable->lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  release(&ptable->lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;
  
  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;
  
  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];
  
  p = allocproc(INIT_PRIORITY);
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  
  sz = proc->sz;
  if(n > 0){
    if((sz = allocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(proc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  proc->sz = sz;
  switchuvm(proc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;

  // Allocate process.
  int child_priority = proc->priority;
  if((np = allocproc(child_priority)) == 0)
    return -1;

  // Copy process state from p.
  if((np->pgdir = copyuvm(proc->pgdir, proc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = proc->sz;
  np->parent = proc;
  *np->tf = *proc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(proc->ofile[i])
      np->ofile[i] = filedup(proc->ofile[i]);
  np->cwd = idup(proc->cwd);
 
  pid = np->pid;
  np->state = RUNNABLE;
  safestrcpy(np->name, proc->name, sizeof(proc->name));
  return pid;
}

void assign_parent_as_init(process_queue *ptable)
{
    struct proc *p;
    process_queue *init_table = get_ptable(INIT_PRIORITY);
    acquire(&ptable->lock);
    if (init_table != ptable) {
        acquire(&init_table->lock);
    }
  // Parent might be sleeping in wait().
  wakeup1(proc->parent, ptable);

  // Pass abandoned children to init.
  for(p = ptable->proc; p < &ptable->proc[NPROC]; p++){
    if(p->parent == proc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc, init_table);
    }
  }
  if (init_table != ptable) {
      release(&init_table->lock);
  }
  release(&ptable->lock);
  
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  int fd;
  process_queue *ptable = get_ptable(proc->priority);

  if(proc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(proc->ofile[fd]){
      fileclose(proc->ofile[fd]);
      proc->ofile[fd] = 0;
    }
  }

  iput(proc->cwd);
  proc->cwd = 0;

  assign_parent_as_init(&ptable0);
  assign_parent_as_init(&ptable1);


  acquire(&ptable->lock);
  // Jump into the scheduler, never to return.
  proc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

int search_for_children(process_queue *ptable)
{
    struct proc *p;
    int havekids = -1;
    int pid;
    acquire(&ptable->lock);
    for(p = ptable->proc; p < &ptable->proc[NPROC]; p++){
      if(p->parent != proc)
        continue;
      havekids = 0;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->state = UNUSED;
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        release(&ptable->lock);
        return pid;
      }
    }
    release(&ptable->lock);
    return havekids;

}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  process_queue *my_ptable = get_ptable(proc->priority);

  for(;;){
    // Scan through table looking for zombie children.
    int pid = search_for_children(&ptable0);
    if (pid < 0) {
        pid = search_for_children(&ptable1);
    }

    if (pid > 0) {
        return pid;
    }
    
    // No point waiting if we don't have any children.
    acquire(&my_ptable->lock);
    if(pid == -1 || proc->killed){
      release(&my_ptable->lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(proc, &my_ptable->lock);  //DOC: wait-sleep
    release(&my_ptable->lock);
  }
}

void round_robin_ptable(process_queue *ptable)
{
    struct proc *p;
    acquire(&ptable->lock);
    for(p = ptable->proc; p < &ptable->proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      proc = p;
      switchuvm(p);
      p->state = RUNNING;
      swtch(&cpu->scheduler, proc->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      proc = 0;
    }
    release(&ptable->lock);

}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    round_robin_ptable(&ptable0);
    round_robin_ptable(&ptable1);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state.
void
sched(void)
{
  int intena;
  process_queue *ptable = get_ptable(proc->priority);

  if(!holding(&ptable->lock))
    panic("sched ptable.lock");
  if(cpu->ncli != 1)
    panic("sched locks");
  if(proc->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = cpu->intena;
  swtch(&proc->context, cpu->scheduler);
  cpu->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  process_queue *ptable = get_ptable(proc->priority);
  acquire(&ptable->lock);  //DOC: yieldlock
  proc->state = RUNNABLE;
  sched();
  release(&ptable->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  process_queue *ptable = get_ptable(proc->priority);
  // Still holding ptable.lock from scheduler.
  release(&ptable->lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot 
    // be run from main().
    first = 0;
    initlog();
  }
  
  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  process_queue *ptable = get_ptable(proc->priority);
  if(proc == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable->lock){  //DOC: sleeplock0
    acquire(&ptable->lock);  //DOC: sleeplock1
    release(lk);
  }

  // Go to sleep.
  proc->chan = chan;
  proc->state = SLEEPING;
  sched();

  // Tidy up.
  proc->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable->lock){  //DOC: sleeplock2
    release(&ptable->lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan, process_queue *ptable)
{
  struct proc *p;

  for(p = ptable->proc; p < &ptable->proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable0.lock);
  wakeup1(chan, &ptable0);
  release(&ptable0.lock);

  acquire(&ptable1.lock);
  wakeup1(chan, &ptable1);
  release(&ptable1.lock);
}

int search_for_process_and_kill(process_queue *ptable, int pid)
{
  struct proc *p;

  acquire(&ptable->lock);
  for(p = ptable->proc; p < &ptable->proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable->lock);
      return 0;
    }
  }
  release(&ptable->lock);
  return -1;

}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
    int status;
    status = search_for_process_and_kill(&ptable0, pid);
    if (status == 0) {
        return 0;
    }
    status = search_for_process_and_kill(&ptable1, pid);
    if (status == 0) {
        return 0;
    }
    return -1;
}


void ptable_dump(process_queue *ptable)
{
static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];
  
  for(p = ptable->proc; p < &ptable->proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
    cprintf("Process table priority 0\n");
    ptable_dump(&ptable0);
    cprintf("Process table priority 1\n");
    ptable_dump(&ptable1);
  
}


