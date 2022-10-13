// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);
void increase_num_ref(uint64 pa);
void decrease_num_ref(uint64 pa);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

int num_ref_to_page[PHYSTOP/PGSIZE];

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
  {
    kfree(p);
  }

}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
  {
    if(num_ref_to_page[(uint64)r/PGSIZE] != 0)
    {
      // panic("kalloc: page is in use");
      release(&kmem.lock);
      return 0;
    }
    num_ref_to_page[(uint64)r/PGSIZE] = 1;
    kmem.freelist = r->next;
  }
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

void increase_num_ref(uint64 pa)
{
  // handle error
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("increase_num_ref");

  // acquire lock
  acquire(&kmem.lock);

  num_ref_to_page[pa/PGSIZE]++;

  release(&kmem.lock);
}

void decrease_num_ref(uint64 pa)
{
  // handle error
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("decrease_num_ref");
  if (num_ref_to_page[pa/PGSIZE] <= 0)
    panic("decrease_num_ref: num_ref_to_page[pa/PGSIZE] == 0");

  // acquire lock
  acquire(&kmem.lock);  

  num_ref_to_page[pa/PGSIZE]--;

  if(num_ref_to_page[pa/PGSIZE] == 0)
  {
    release(&kmem.lock);
    kfree((void*)pa);
    return;
  }

  release(&kmem.lock);
}
