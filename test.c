/*
    compile options:
      -DBKL:  enable big_kernel_lk
      -DDEBUG: enable runtime check and stat output ...
      -DTEST: when compile in native (not in Qemu) ...
    
    compile and run in Qemu: make run ARCH=x86_64-qemu smp=4
*/

#include "threads.h"
#include <assert.h>
#include "pmm.h"
#include <time.h>
#include <signal.h>

#define PAGE_SIZE 8192
#define CPU_NUM 4

#define stat_interval 4096
// #define DEBUG

size_t times[CPU_NUM] = {0};
clock_t clocks[CPU_NUM] = {0};

static void entry(int tid)
{
  assert(tid >= 0 && tid - 1 < CPU_NUM);
  kalloc(tid - 1, 128);
}

static void goodbye()
{
  printf("End.\n");
}

void single_thread_test_body(int tid)
{
  void *p = NULL;
  for (int i = 0; i < 1024; i++)
  {
    assert(tid - 1 >= 0 && tid - 1 < CPU_NUM);
    p = kalloc(tid - 1, rand() % 128);
    kfree(tid - 1, p);
  }
}

enum ops
{
  OP_ALLOC = 0,
  OP_FREE
};
struct malloc_op
{
  enum ops type;
  union
  {
    size_t sz;
    size_t i;
  };
  void *addr;
};

#define MAX_OP_NUM 1024

int malloc_num[CPU_NUM] = {0, 0, 0, 0};
struct malloc_op *malloc_pool[CPU_NUM][MAX_OP_NUM];
spinlock_t lk[CPU_NUM];
struct malloc_op *random_op(int tid, size_t min_sz, size_t delta)
{
  struct malloc_op *c = (struct malloc_op *)malloc(sizeof(struct malloc_op));
  if (malloc_num[tid - 1] < MAX_OP_NUM)
  {
    int op_type = rand() % 2;
    if (op_type == OP_ALLOC || malloc_num[tid - 1] == 0)
    { 
      if (delta != 0) {
        *c = (struct malloc_op){
            .type = OP_ALLOC,
            .sz = min_sz + (rand() % delta),
            .addr = NULL,
        };
      }
      else {
        *c = (struct malloc_op){
            .type = OP_ALLOC,
            .sz = min_sz,
            .addr = NULL,
        };
      }
    }
    else if (op_type == OP_FREE)
    {
      int op_i = rand() % (malloc_num[tid - 1]);
      *c = (struct malloc_op){
          .type = OP_FREE,
          .addr = malloc_pool[tid - 1][op_i]->addr,
          .i = op_i,
      };
    }
  }
  else
  {
    int op_i = rand() % (malloc_num[tid - 1] - 1);
    *c = (struct malloc_op){
        .type = OP_FREE,
        .addr = malloc_pool[tid - 1][op_i]->addr,
        .i = op_i,
    };
  }
  return c;
}

size_t test_stat() {
  size_t used_sz = 0;
  for (int i = 0; i < CPU_NUM; i++) {
    for (int j = 0; j < malloc_num[i]; j++) {
      if (malloc_pool[i][j]->type == OP_ALLOC) {
        int k = 0;
        for (; k < 32; k++)
          if (malloc_pool[i][j]->sz <= (2 << k))
            break;
        size_t sz_ = 2 << k;
        if (sz_ + sizeof(alloc_header) < sizeof(free_node))
        {
          sz_ = sizeof(free_node) - sizeof(alloc_header);
        }
        if (sz_ < PAGE_SIZE)
          used_sz += sz_;
        else
          used_sz += malloc_pool[i][j]->sz;
      }
    }
  }
  return used_sz;
}

void stat_output(int i) {
  if (i % stat_interval == 0) {
    for (int i = 0; i < CPU_NUM; i++) {
      spin_lock(&(lk[i]));
      spin_lock(&(cpu_page_list[i]->HDR.lock));
    }
    spin_lock(&(Mem_freenode_head.lk));
    mem_stat *mp = NULL;
    double test_used = test_stat() / 1024.0 / 1024.0;
    printf("[TEST] used_sz = %8f MB\n", test_used);
    mp = memory_stat();
    double real_small_used = (mp->page_num * (PAGE_SIZE - sizeof(header_t)) - mp->small_malloc_sz) / 1024.0 / 1024.0;
    double real_big_used = (HEAP_SIZE - mp->page_num * PAGE_SIZE - mp->big_malloc_sz) / 1024.0 / 1024.0;
    assert(test_used == real_small_used + real_big_used);
    printf("[REAL] used_sz = %8f MB\n", real_small_used + real_big_used);

    for (int i = 0; i < CPU_NUM; i++) {
      spin_unlock(&(lk[i]));
      spin_unlock(&(cpu_page_list[i]->HDR.lock));
    }
    spin_unlock(&(Mem_freenode_head.lk));
  }
}

void time_reportor(){
  printf("[CPU 0]: %f\n", (double)(clocks[0])/CLOCKS_PER_SEC);
  printf("real time: %f\n", (double)(clocks[0])/CLOCKS_PER_SEC);
}

void muti_time_reportor(){
  for (int i = 0; i < CPU_NUM; i++)
    printf("[CPU %i]: %f\n", i, (double)(clocks[i])/CLOCKS_PER_SEC);
  double clock_sum = 0;
  for (int i = 0; i < CPU_NUM; i++)
    clock_sum += clocks[i];
  printf("real time: %f\n", (clock_sum/CPU_NUM)/CLOCKS_PER_SEC);
}

void small_memory_stress_test_body(int tid)
{
  int i = 0;
  while (1) {
    spin_lock(&lk[tid - 1]);
    struct malloc_op *op = random_op(tid, 0, 128);
    if (op->type == OP_ALLOC)
    {
      op->addr = kalloc(tid - 1, op->sz);
      if (op->addr == NULL) {
        free(op);
      }
      else {
        malloc_pool[tid - 1][malloc_num[tid - 1]] = op;
        malloc_num[tid - 1]++;
        i++;
      }
    }
    else if (op->type == OP_FREE)
    {
      kfree(tid - 1, op->addr);
      free(malloc_pool[tid - 1][op->i]);
      malloc_pool[tid - 1][op->i] = malloc_pool[tid - 1][malloc_num[tid - 1] - 1];
      malloc_num[tid - 1]--;
      i++;
    }
    spin_unlock(&lk[tid - 1]);
#ifdef DEBUG
    stat_output(i);
#endif
    if (i == (1<<18))
      break;
  }
}

void big_memory_stress_test_body(int tid) {
  int i = 0;
  while (1) {
    spin_lock(&lk[tid - 1]);
    struct malloc_op *op = random_op(tid, PAGE_SIZE, 4*PAGE_SIZE);
    if (op->type == OP_ALLOC)
    {
      op->addr = kalloc(tid - 1, op->sz);
      if (op->addr == NULL) {
        free(op);
      }
      else {
        malloc_pool[tid - 1][malloc_num[tid - 1]] = op;
        malloc_num[tid - 1]++;
        i++;
      }
    }
    else if (op->type == OP_FREE)
    {
      kfree(tid - 1, op->addr);
      free(malloc_pool[tid - 1][op->i]);
      malloc_pool[tid - 1][op->i] = malloc_pool[tid - 1][malloc_num[tid - 1] - 1];
      malloc_num[tid - 1]--;
      i++;
    }
    spin_unlock(&lk[tid - 1]);
#ifdef DEBUG
    stat_output(i);
#endif
    if (i == (1<<18))
      break;
  }
}

void mix_stress_test_body(int tid) {
  int i = 0;
  while (1) {
    spin_lock(&lk[tid - 1]);
    struct malloc_op *op = random_op(tid, 0, 4*PAGE_SIZE);
    if (op->type == OP_ALLOC)
    {
      op->addr = kalloc(tid - 1, op->sz);
      if (op->addr == NULL) {
        free(op);
      }
      else {
        malloc_pool[tid - 1][malloc_num[tid - 1]] = op;
        malloc_num[tid - 1]++;
        i++;
      }
    }
    else if (op->type == OP_FREE)
    {
      kfree(tid - 1, op->addr);
      free(malloc_pool[tid - 1][op->i]);
      malloc_pool[tid - 1][op->i] = malloc_pool[tid - 1][malloc_num[tid - 1] - 1];
      malloc_num[tid - 1]--;
      i++;
    }
    spin_unlock(&lk[tid - 1]);
#ifdef DEBUG
    stat_output(i);
#endif
    if (i == (1<<18))
      break;
  }
}

void restrict_test_body(int tid) {
  int i = 0;
  while (1) {
    spin_lock(&lk[tid - 1]);
    struct malloc_op *op = random_op(tid, sizeof(uintptr_t), 4*PAGE_SIZE);
    // param min_sz must be sizeof(uintptr_t) ...  for checking ...
    if (op->type == OP_ALLOC)
    {
      op->addr = kalloc(tid - 1, op->sz);
      *(uintptr_t *)(op->addr) = (uintptr_t)(op->addr);
      int j = 0;
      for (; j < 32; j++)
        if (op->sz <= (2 << j))
          break;
      size_t sz_ = 2 << j;
      if (sz_ < PAGE_SIZE)
      // here is the checking ...
        *(uintptr_t *)(op->addr+sz_-sizeof(uintptr_t)) = (uintptr_t)(op->addr);
      else
        *(uintptr_t *)(op->addr+op->sz-sizeof(uintptr_t)) = (uintptr_t)(op->addr);
      if (op->addr == NULL) {
        free(op);
      }
      else {
        malloc_pool[tid - 1][malloc_num[tid - 1]] = op;
        malloc_num[tid - 1]++;
        i++;
        times[tid - 1] ++;
      }
    }
    else if (op->type == OP_FREE)
    {
#ifdef DEBUG
      assert(*(uintptr_t *)(op->addr) == (uintptr_t)(op->addr));
      int j = 0;
      for (; j < 32; j++)
        if (malloc_pool[tid - 1][op->i]->sz <= (2 << j))
          break;
      size_t sz_ = 2 << j;
      if (sz_ < PAGE_SIZE)
        assert(*(uintptr_t *)(op->addr+sz_-sizeof(uintptr_t)) == (uintptr_t)(op->addr));
      else
        assert(*(uintptr_t *)(op->addr+malloc_pool[tid - 1][op->i]->sz-sizeof(uintptr_t)) == (uintptr_t)(op->addr));
#endif
      kfree(tid - 1, op->addr);
      free(malloc_pool[tid - 1][op->i]);
      malloc_pool[tid - 1][op->i] = malloc_pool[tid - 1][malloc_num[tid - 1] - 1];
      malloc_num[tid - 1]--;
      i++;
    }
    spin_unlock(&lk[tid - 1]);
#ifdef DEBUG
    stat_output(i);
#endif
    if (i == (1<<18))
      break;
  }
}


#ifdef BKL
spinlock_t big_kernel_lk = (spinlock_t) {.locked = 0};
#endif

void perf_body(int tid) {
  int i = 0;
  struct malloc_op *op;
  double sd = 0;
  clock_t ct;
  while (1) {
    spin_lock(&lk[tid - 1]);
    sd = rand() / (RAND_MAX + 1.0);
    if (sd < 0.5)
      op = random_op(tid, 0, 128);
    else if (sd < 0.95)
      op = random_op(tid, 4096, 0);
    else
      op = random_op(tid, PAGE_SIZE, 4*PAGE_SIZE);
    if (op->type == OP_ALLOC)
    {
      ct = clock();
#ifdef BKL
      spin_lock(&big_kernel_lk);
#endif
      op->addr = kalloc(tid - 1, op->sz);
#ifdef BKL
      spin_unlock(&big_kernel_lk);
#endif
      clocks[tid-1] += (clock() - ct);
      if (op->addr == NULL) {
        free(op);
      }
      else {
        malloc_pool[tid - 1][malloc_num[tid - 1]] = op;
        malloc_num[tid - 1]++;
        i++;
      }
    }
    else if (op->type == OP_FREE)
    {
      ct = clock();
#ifdef BKL
      spin_lock(&big_kernel_lk);
#endif
      kfree(tid - 1, op->addr);
#ifdef BKL
      spin_unlock(&big_kernel_lk);
#endif
      clocks[tid-1] += (clock() - ct);
      free(malloc_pool[tid - 1][op->i]);
      malloc_pool[tid - 1][op->i] = malloc_pool[tid - 1][malloc_num[tid - 1] - 1];
      malloc_num[tid - 1]--;
      free(op);
      i++;
    }
    spin_unlock(&lk[tid - 1]);

    if (i == (1<<18))
      break;
  }
}

void perf_frame(int tid) {
  int i = 0;
  struct malloc_op *op;
  double sd = 0;
  clock_t ct;
  while (1) {
    spin_lock(&lk[tid - 1]);
    sd = rand() / (RAND_MAX + 1.0);
    if (sd < 0.5)
      op = random_op(tid, 0, 128);
    else if (sd < 0.95)
      op = random_op(tid, 4096, 0);
    else
      op = random_op(tid, PAGE_SIZE, 4*PAGE_SIZE);
    if (op->type == OP_ALLOC)
    {
      ct = clock();
      // op->addr = kalloc(tid - 1, op->sz);
      clocks[tid-1] += (clock() - ct);

      malloc_pool[tid - 1][malloc_num[tid - 1]] = op;
      malloc_num[tid - 1]++;
      i++;
  
    }
    else if (op->type == OP_FREE)
    {
      ct = clock();
      // kfree(tid - 1, op->addr);
      clocks[tid-1] += (clock() - ct);
      free(malloc_pool[tid - 1][op->i]);
      malloc_pool[tid - 1][op->i] = malloc_pool[tid - 1][malloc_num[tid - 1] - 1];
      malloc_num[tid - 1]--;
      free(op);
      i++;
    }
    spin_unlock(&lk[tid - 1]);

    if (i == (1<<18))
      break;
  }
}

void single_thread_small_memory_stress_test() {
  pmm_init();
  for (int i = 0; i < 1; i++)
    create(small_memory_stress_test_body);
  join(goodbye);
}

void muti_threads_small_memory_stress_test() {
  pmm_init();
  for (int i = 0; i < CPU_NUM; i++)
    create(small_memory_stress_test_body);
  join(goodbye);
}

void final_reporter(int signo) {
  if (signo = SIGILL) {
    for (int i = 0; i < CPU_NUM; i++)
      printf("[cpu %d]: %u\n", i, times[i]);
  }
} 

void single_thread_big_memory_stress_test() {
  pmm_init();
  for (int i = 0; i < 1; i++)
    create(big_memory_stress_test_body);
  join(goodbye);
}

void muti_threads_big_memory_stress_test() {
  pmm_init();
  for (int i = 0; i < CPU_NUM; i++)
    create(big_memory_stress_test_body);
  join(goodbye);
}

void single_thread_mix_stress_test() {
  pmm_init();
  for (int i = 0; i < 1; i++)
    create(mix_stress_test_body);
  join(goodbye);
}

void muti_threads_mix_stress_test() {
  pmm_init();
  for (int i = 0; i < CPU_NUM; i++)
    create(mix_stress_test_body);
  join(goodbye);
}

void single_thread_restrict_test()
{
  pmm_init();
  for (int i = 0; i < 1; i++)
    create(restrict_test_body);
  join(goodbye);
}

void muti_threads_restrict_test() {
  pmm_init();
  for (int i = 0; i < CPU_NUM; i++)
    create(restrict_test_body);
  join(goodbye);
}

void single_thread_perf() {
  pmm_init();
  create(perf_body);
  join(goodbye);
}

void muti_threads_perf() {
  pmm_init();
  for (int i = 0; i < CPU_NUM; i++)
    create(perf_body);
  join(goodbye);
}

void muti_empty_cc() {
  pmm_init();
  for (int i = 0; i < CPU_NUM; i++)
    create(perf_frame);
  join(goodbye);
}

void gen_workload(int tid) {
  struct malloc_op *op = NULL;
  double sd = 0;
  size_t opnum = 1 << 18;
  FILE *f = fopen("/home/javis/os-workbench/kernel/workload", "w+");
  for (int i = 0; i < opnum; i++) {
    sd = rand() / (RAND_MAX + 1.0);
    if (sd < 0.5) {
      op = random_op(tid, 0, 128);
    }
    else if (sd < 0.95) {
      op = random_op(tid, 4096, 0);
    }
    else {
      op = random_op(tid, PAGE_SIZE, 4*PAGE_SIZE);
    }
    if (op->type == OP_ALLOC) {
      if (op->sz != 0) {
        fprintf(f, "A %u\n", op->sz);
        malloc_pool[tid - 1][malloc_num[tid - 1]] = op;
        malloc_num[tid-1]++;
      }
    }
    else if (op->type == OP_FREE) {
      fprintf(f, "F %u %u\n", op->addr, op->i);
      free(malloc_pool[tid - 1][op->i]);
      malloc_pool[tid - 1][op->i] = malloc_pool[tid - 1][malloc_num[tid - 1] - 1];
      malloc_num[tid - 1]--;
      free(op);
    }
    else {
      assert(0);
    }
  }
  fclose(f);
}

int main(int argc, char *argv[])
{
  if (argc < 2)
    exit(1);
  // signal(SIGILL, final_reporter);
  for (int i = 0; i < CPU_NUM; i++)
  {
    spin_init(&lk[i]);
  }
  switch (atoi(argv[1]))
  {
  case 1:
    single_thread_small_memory_stress_test();
    break;
  case 2:
    muti_threads_small_memory_stress_test();
    break;
  case 3:
    single_thread_big_memory_stress_test();
    break;
  case 4:
    muti_threads_big_memory_stress_test();
    break;
  case 5:
    single_thread_mix_stress_test();
    break;
  case 6:
    muti_threads_mix_stress_test();
    break;
  case 7:
    single_thread_restrict_test();
    break;
  case 8:
    muti_threads_restrict_test();
    break;
  case 9:
    single_thread_perf();
    break;
  case 10:
    muti_threads_perf();
    break;
  case 11:
    muti_empty_cc();
    break;
  default:
    assert(0);
  }
  return 0;
}