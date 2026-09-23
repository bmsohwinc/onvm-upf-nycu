/* Test substitute: preserve mutual exclusion without requiring DPDK. */
#ifndef TEST_RTE_SPINLOCK_H
#define TEST_RTE_SPINLOCK_H
#include <assert.h>
#include <pthread.h>
typedef pthread_mutex_t rte_spinlock_t;
#define RTE_SPINLOCK_INITIALIZER PTHREAD_MUTEX_INITIALIZER
static inline void rte_spinlock_lock(rte_spinlock_t *lock) { assert(!pthread_mutex_lock(lock)); }
static inline void rte_spinlock_unlock(rte_spinlock_t *lock) { assert(!pthread_mutex_unlock(lock)); }
#endif
