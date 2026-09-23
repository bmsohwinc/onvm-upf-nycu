# Worker-slot unit tests

From the repository root, with a C compiler and libyaml development files:

```sh
cc -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Itests/worker_slots/stubs -Ionvm/upf -I5gc/upf_c \
  tests/worker_slots/test_worker_slots.c \
  5gc/upf_c/upf_worker_config.c onvm/upf/upf_worker.c \
  -lyaml -o /tmp/test_worker_slots
/tmp/test_worker_slots
```

The tests build YAML documents with libyaml's DOM API and exercise the actual
configuration parser. They cover defaults, decimal/hex integers, malformed and
duplicate fields, resource collisions, pool bounds, output preservation on
failure, registry ownership/ABI checks and one-time configuration publication.
DPDK memzone/process APIs are stubbed; this does not test EAL or hardware.
