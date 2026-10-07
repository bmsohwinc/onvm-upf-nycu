#pragma once
#include <pthread.h>
typedef pthread_rwlock_t rte_rwlock_t;
#define RTE_RWLOCK_INITIALIZER PTHREAD_RWLOCK_INITIALIZER
static inline void rte_rwlock_read_lock(rte_rwlock_t *lock) { pthread_rwlock_rdlock(lock); }
static inline void rte_rwlock_read_unlock(rte_rwlock_t *lock) { pthread_rwlock_unlock(lock); }
static inline void rte_rwlock_write_lock(rte_rwlock_t *lock) { pthread_rwlock_wrlock(lock); }
static inline void rte_rwlock_write_unlock(rte_rwlock_t *lock) { pthread_rwlock_unlock(lock); }
