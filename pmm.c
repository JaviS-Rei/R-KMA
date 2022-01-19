#include <stdint.h>
#include "pmm.h"
#define CPU_NUM 4

void pmm_init() {
  char *ptr  = malloc(HEAP_SIZE);
  heap.start = ptr;
  heap.end   = ptr + HEAP_SIZE;
  printf("Got %d MiB heap: [%p, %p)\n", HEAP_SIZE >> 20, heap.start, heap.end);
  Mem_freenode_head.addr = heap.start;
  Mem_freenode_head.obj_cnt = 0;
  spin_init(&(Mem_freenode_head.lk));
  *(Mem_freenode_head.addr) = (free_node) {
    .start = heap.start,
    .len = HEAP_SIZE,
    .prev = NULL,
    .next = NULL,
  };
  for (int i = 0; i < 4; i++) {
    // memmove((void *)((uintptr_t)Mem_freenode_head + PAGE_SIZE), Mem_freenode_head, sizeof(free_node));
    cpu_page_list[i] = page_alloc(i);
  }
}

void *kalloc(int tid, size_t size) {
  int i = 0;
  for (; i < 32; i++)
    if (size <= (2 << i))
      break;
  size_t small_size = 2 << i;
  void *p = NULL;
  if (small_size < PAGE_SIZE) {
    spin_lock(&(cpu_page_list[tid]->HDR.lock));
    p = split_alloc(cpu_page_list[tid], small_size, 0, tid);    
    spin_unlock(&(cpu_page_list[tid]->HDR.lock));
  }
  else {
    spin_lock(&(Mem_freenode_head.lk));
    p = BIGMEM_split_alloc(size);
    spin_unlock(&(Mem_freenode_head.lk));
  }
  return p;
}

void kfree(int tid, void *ptr) {
  alloc_header *ah = ptr - sizeof(alloc_header);
  if (ah->cpu_id == -1) {
    BIGMEM_coalescing_free(ptr);
  }
  else {
    page_t *tmp_p = cpu_page_list[ah->cpu_id];
    while (tmp_p != NULL) {
      if ((uintptr_t)tmp_p <= (uintptr_t)ptr && (uintptr_t)ptr < (uintptr_t)tmp_p + PAGE_SIZE) {
        break;
      }
      tmp_p = (page_t *)(tmp_p->HDR.nextpage);
    }
    if (tmp_p == NULL) {
      printf("abnormal free, ptr hasn't been allocated.\n");
      assert(0);
    }
    coalescing_free(tmp_p, ptr);
  }
}




