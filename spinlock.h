#include <stdint.h>

static inline intptr_t atomic_xchg_(volatile intptr_t *addr,
                               intptr_t newval) {
  intptr_t result;
  asm volatile ("lock xchg %0, %1":
    "+m"(*addr), "=a"(result) : "1"(newval) : "cc");
  return result;
}

typedef struct spinlock {
  intptr_t locked;
} spinlock_t;

static void spin_init(spinlock_t *lk) {
  lk->locked = 0;
}

static void spin_lock(spinlock_t *lk) {
  while (atomic_xchg_(&lk->locked, 1)) ;
}

static void spin_unlock(spinlock_t *lk) {
  atomic_xchg_(&lk->locked, 0);
}
