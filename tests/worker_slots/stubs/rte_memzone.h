#pragma once
#include <stddef.h>
#define SOCKET_ID_ANY -1
struct rte_memzone { void *addr; size_t len; };
const struct rte_memzone *rte_memzone_lookup(const char *name);
const struct rte_memzone *rte_memzone_reserve(const char *name, size_t size, int socket, unsigned flags);
