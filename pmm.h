#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "spinlock.h"
#define HEAP_SIZE 1024u*1024u*1024u
#define PAGE_SIZE 8192
#define HDR_SIZE sizeof(header_t)

#define LinkListCheck(p)                         \
  assert(p->next == NULL || p->next->prev == p); \
  assert(p->prev == NULL || p->prev->next == p);

typedef union page page_t;
typedef struct header header_t;

// ============== page list ===============

// typedef struct pagelist page_list;
// struct pagelist{
//   page_t *list;
//   page_list *next;
// };

// ============= alloc header ==============

typedef struct
{
  int  cpu_id;
  uint32_t len;
  uint32_t magic;
} alloc_header;

// ============== free list ==============

typedef struct freenode free_node;
struct freenode
{
  void *start;
  uint32_t len;
  free_node *prev;
  free_node *next;
};

typedef struct freelist free_list;
struct freelist
{
  free_node *head;
};

struct header
{
  spinlock_t lock;    // 锁，用于串行化分配和并发的 free
  int obj_cnt;        // 页面中已分配的对象数，减少到 0 时回收页面
  header_t *nextpage; // 属于同一个线程的 *页面的链表*
  free_list freelist;
} __attribute__((packed));

union page
{
  header_t HDR;
  struct
  {
    uint8_t header[HDR_SIZE];
    uint8_t data[PAGE_SIZE - HDR_SIZE];
  } __attribute__((packed));
};

struct {
  free_node *addr;
  spinlock_t lk;
  int obj_cnt;
} Mem_freenode_head;

#ifdef TEST // memmove
#include <string.h>
#include <stdio.h>
#include <assert.h>
#endif

static free_node *freenode_walker(free_node *p, size_t size) {
  while (p != NULL) {
    LinkListCheck(p);
    if (p->len >= (size + sizeof(alloc_header)) + sizeof(free_node)) {
      return p;
    }
    p = p->next;
  }
  return NULL;
}

static void *policy_FirstFit(page_t **p, size_t size) {
  if (size + sizeof(alloc_header) < sizeof(free_node)) {
    size = sizeof(free_node) - sizeof(alloc_header);
  }

  free_node *p_free_node = (*p)->HDR.freelist.head;
  while (1) {
    p_free_node = freenode_walker(p_free_node, size);
    if (p_free_node != NULL)
      return p_free_node;
    if ((*p)->HDR.nextpage != NULL) {
      (*p) = (page_t *)((*p)->HDR.nextpage);
      p_free_node = (*p)->HDR.freelist.head;
    }
    else
      break;
  }
  return NULL;
}

static void *BIGMEM_split_alloc(size_t size) {
  free_node *fp = freenode_walker(Mem_freenode_head.addr, size);
  if (fp == NULL) {
    return NULL;
  }
  free_node *new_fp = (free_node *)((uintptr_t)fp + size + sizeof(alloc_header));
  memmove(new_fp, fp, sizeof(free_node));
  new_fp->len -= (size + sizeof(alloc_header));
  new_fp->start = (void *)((uintptr_t)fp + sizeof(alloc_header));
  if (new_fp->prev == NULL) {
    // FIXME: DATA RACE ...
    Mem_freenode_head.addr = new_fp;
    if (new_fp->next != NULL)
      new_fp->next->prev = Mem_freenode_head.addr;
  }
  else {
    fp->prev->next = new_fp;
    if (new_fp->next != NULL)
      fp->next->prev = new_fp;
  }
  LinkListCheck(new_fp);
  *(alloc_header *)fp = (alloc_header) {
    .cpu_id = -1,
    .len = size,
    .magic = 0x6d616c63,
  };
  Mem_freenode_head.obj_cnt ++;
  assert(new_fp->len <= HEAP_SIZE);
  void *up = (void *)((uintptr_t)fp + sizeof(alloc_header));
  return up;
}

static page_t *page_alloc(int tid)
{
  // if ((uintptr_t)p + PAGE_SIZE >= (uintptr_t)heap.end)
  //   return NULL;
  spin_lock(&(Mem_freenode_head.lk));
  void *p = BIGMEM_split_alloc(PAGE_SIZE);
  spin_unlock(&(Mem_freenode_head.lk));
  if (p == NULL)
    return NULL;
  *(page_t *)(p) = (page_t){
    .HDR = (header_t){
        .obj_cnt = 0,
        .nextpage = NULL,
        .freelist = (free_list){
            .head = (free_node *)((uintptr_t)p + sizeof(spinlock_t) + sizeof(int) + sizeof(header_t *) + sizeof(free_list)),
        }
    }
  };
  *(((page_t *)p)->HDR.freelist.head) = (free_node){
    .start = p + HDR_SIZE,
    .len = PAGE_SIZE - 28,
    .next = NULL,
    .prev = NULL,
  };
  spin_init(&(((page_t *)p)->HDR.lock));
  return (page_t *)p;
}

static void _free(free_node **p, void *ptr) {
  alloc_header *ah = ptr - sizeof(alloc_header);
  assert(ah->magic == 0x6d616c63);
  size_t sz = ah->len;
  free_node *fp = (free_node *)ah;
  fp->len = sz + sizeof(alloc_header);
  fp->start = (void *)fp + sizeof(free_node);

  free_node *tp = *p;
  
  // freenode address layout: low -> high ..
  if (tp == NULL) {
    *p = fp;
    fp->next = fp->prev = NULL;
    goto COALESCING;
  }

  if (fp < tp) {
    fp->prev = NULL;
    fp->next = tp;
    tp->prev = fp;
    *p = fp;
    goto COALESCING;
  }

  while (tp->next != NULL) {
    assert((uintptr_t)fp != (uintptr_t)(tp->next));
    assert(tp->len <= HEAP_SIZE);
    if (((uintptr_t)tp + tp->len) <= (uintptr_t)fp && (uintptr_t)fp < (uintptr_t)(tp->next)) {
      fp->prev = tp;
      fp->next = tp->next;
      // adjust fp->prev
      fp->prev->next = fp;
      // adjust fp->next
      fp->next->prev = fp;
      goto COALESCING;
    }
    tp = tp->next;
  }
  
  /*
   *   |       len       |
   *   ----------------------------------------------------------
   *   |                 |    alloced   | .. |   alloced   | .. | 
   *   ----------------------------------------------------------
   *   ^                                     ^
   *   tp                                    fp
   * 
   *   ATTENTION: tp is the only free_node (next == NULL)
   */
  assert((uintptr_t)tp + tp->len <= (uintptr_t)fp);
  if (tp->next == NULL) {
    fp->next = NULL;
    fp->prev = tp;
    tp->next = fp;
    goto COALESCING;
  }

  // compare fp, fp->prev and fp->next.
COALESCING:
  if (fp->prev != NULL && (uintptr_t)fp == (uintptr_t)fp->prev + fp->prev->len) {
    fp->prev->len += fp->len;
    fp->prev->next = fp->next;
    fp->prev->next->prev = fp->prev;
    LinkListCheck(fp->prev);
  }
  else if ((uintptr_t)fp + fp->len == (uintptr_t)fp->next) {
    // FIXME: ...
    fp->len += fp->next->len;
    fp->next = fp->next->next;
    if (fp->next != NULL)
      fp->next->prev = fp;
    LinkListCheck(fp);
  }
  if (fp->prev != NULL && (uintptr_t)fp->prev + fp->prev->len == (uintptr_t)fp->next) {
    fp->prev->len += fp->next->len;
    fp->prev->next = fp->next->next;
    if (fp->prev->next != NULL)
      fp->prev->next->prev = fp->prev;
    LinkListCheck(fp->prev);
  }
}

static void BIGMEM_coalescing_free(void *ptr) {
  spin_lock(&(Mem_freenode_head.lk));
  _free(&(Mem_freenode_head.addr), ptr);
  Mem_freenode_head.obj_cnt--;
  spin_unlock(&(Mem_freenode_head.lk));
}

// static void page_free(page_t *page) {
//   // TODO: ...  
// }

static void coalescing_free(page_t *page, void* ptr)
{
  spin_lock(&(page->HDR.lock));
  _free(&(page->HDR.freelist.head), ptr);
  page->HDR.obj_cnt --;
  // if (page->HDR.obj_cnt == 0)
  //   page_free(page);
  spin_unlock(&(page->HDR.lock));
}

static void *split_alloc(page_t *page, size_t size, int recusive_flag, int tid) {
  page_t *t_p_page = page;
  
  // seperate policy and mechanism
  free_node *p = policy_FirstFit(&t_p_page, size);
  /*
    OSTEP implementation: alloc_header will replace free_node and free_node be moved to the end of user_data_section.

    | (free_node).len  |
    --------------------
    |     32    |      |           |      8       |<- size  ->|     32    |
    --------------------           -----------------------------------------------
    | free_node | .... |    ==>    | alloc_header | user data | free_node | .... |
    --------------------           -----------------------------------------------
    ^                              ^              ^           ^
    p                              p              up         nfnp 
    ==============================================================================
    up: user data pointer 
    nfnp: new free node pointer


    |<------------------------- 8192 - HDR_SIZE --------------------------->|
    -------------------------------------------------------------------------
    |  alloc_header  |  ...  |  alloc_header  |  ...  |  ...  |  free_node  |
    -------------------------------------------------------------------------

  */
  void *up = (void *)((uintptr_t)p + sizeof(alloc_header));

  if (size + sizeof(alloc_header) < sizeof(free_node)) {
    size = sizeof(free_node) - sizeof(alloc_header);
  }

  free_node *nfnp = (free_node *)((uintptr_t)up + size);
  if (p == NULL) {
    page_t *page_iter = t_p_page;
    while (page_iter->HDR.nextpage != NULL) {
      page_iter = (page_t *)page_iter->HDR.nextpage;
    }
    // FIXME: DATA RACE ...
    page_iter->HDR.nextpage = (header_t *)page_alloc(tid);
    if (page_iter->HDR.nextpage == NULL) {
      return NULL;
    }

    return split_alloc((page_t *)page_iter->HDR.nextpage, size, 1, tid);
  }

  assert((uintptr_t)nfnp + sizeof(free_node) <= (uintptr_t)t_p_page + PAGE_SIZE);
  memmove(nfnp, p, sizeof(free_node));
  nfnp->len -= (size + sizeof(alloc_header));
  assert((uintptr_t)t_p_page <= (uintptr_t)nfnp && (uintptr_t)nfnp + nfnp->len <= (uintptr_t)t_p_page + PAGE_SIZE);
  assert(nfnp->len >= 0);

  // FIXME: ...
  if (nfnp->len == 0) {
    if (nfnp->prev != NULL) {
      nfnp->prev->next = nfnp->next;
      if (nfnp->prev->next != NULL)
        nfnp->prev->next->prev = nfnp->prev;
      LinkListCheck(nfnp->prev);
    }
    else if (nfnp->prev == NULL) {
      t_p_page->HDR.freelist.head = nfnp->next;
      if (t_p_page->HDR.freelist.head->next != NULL)
        t_p_page->HDR.freelist.head->next->prev = t_p_page->HDR.freelist.head;
      LinkListCheck(t_p_page->HDR.freelist.head);
    }
  }

  if (nfnp->prev == NULL) {
    t_p_page->HDR.freelist.head = nfnp;
    if (nfnp->next != NULL)
      nfnp->next->prev = nfnp;
    LinkListCheck(t_p_page->HDR.freelist.head);
  }
  else {
    nfnp->prev->next = nfnp;
    if (nfnp->next != NULL)
      nfnp->next->prev = nfnp;
  }

  t_p_page->HDR.obj_cnt++;
  *((alloc_header *)p) = (alloc_header){
      .cpu_id = tid,
      .len = size,
      .magic = 0x6d616c63, // m: 6d  a: 61  l:6c  c:63  ==>  mal(lo)c
  };
  assert(nfnp->len <= PAGE_SIZE);
  // printf("page: %p, obj_id: %d, leave critical section :: kalloc size: %ld, kalloc return: %p\n", t_p_page, t_p_page->HDR.obj_cnt - 1, size, up);
  return up;
}

page_t *cpu_page_list[128];

#define CPU_NUM 4
typedef struct {
  size_t small_malloc_sz;
  size_t big_malloc_sz;
  size_t page_num;
} mem_stat;

#ifdef TEST
static mem_stat *memory_stat() {
  mem_stat *ms = (mem_stat *)malloc(sizeof(mem_stat));
  *ms = (mem_stat) {
    .page_num = 0,
    .small_malloc_sz = 0,
    .big_malloc_sz = 0,
  };

  int malloc_n = 0;
  free_node *fnode_p = NULL;

  page_t *page_p = NULL;
  for (int i = 0; i < CPU_NUM; i++) {
    page_p = cpu_page_list[i];
    while (page_p != NULL) {
      malloc_n += page_p->HDR.obj_cnt;
      ms->page_num ++;
      fnode_p = page_p->HDR.freelist.head;
      while (fnode_p != NULL) {
        ms->small_malloc_sz += fnode_p->len;
        fnode_p = fnode_p->next;
      }
      page_p = (page_t *)(page_p->HDR.nextpage);
    }
  }
  ms->small_malloc_sz += malloc_n * sizeof(alloc_header);

  fnode_p = Mem_freenode_head.addr;
  while (fnode_p != NULL) {
    ms->big_malloc_sz += fnode_p->len;
    fnode_p = fnode_p->next;
  }
  ms->big_malloc_sz += Mem_freenode_head.obj_cnt * sizeof(alloc_header);
  return ms;
}
#endif

void pmm_init();
void *kalloc(int tid, size_t size);
void kfree(int tid, void *ptr);

typedef struct {
  void *start, *end;
} Area;

static Area heap = {};