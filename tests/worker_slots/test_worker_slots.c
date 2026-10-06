/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rte_eal.h>
#include <rte_memzone.h>
#include "upf_worker_config.h"

int rte_errno;
static enum rte_proc_type_t process_type = RTE_PROC_PRIMARY;
static UpfWorkerRegistry storage;
static struct rte_memzone zone;
static int zone_exists;

enum rte_proc_type_t rte_eal_process_type(void) { return process_type; }
const struct rte_memzone *rte_memzone_lookup(const char *name) {
    assert(!strcmp(name, MZ_UPF_WORKERS));
    return zone_exists ? &zone : NULL;
}
const struct rte_memzone *rte_memzone_reserve(const char *name, size_t size, int socket, unsigned flags) {
    assert(!strcmp(name, MZ_UPF_WORKERS) && size == sizeof(storage));
    assert(socket == SOCKET_ID_ANY && flags == 0);
    if (zone_exists) { rte_errno = EEXIST; return NULL; }
    memset(&storage, 0xa5, sizeof(storage));
    zone = (struct rte_memzone){.addr = &storage, .len = size};
    zone_exists = 1;
    return &zone;
}

static int Scalar(yaml_document_t *doc, const char *value) {
    return yaml_document_add_scalar(doc, NULL, (yaml_char_t *)value, (int)strlen(value), YAML_PLAIN_SCALAR_STYLE);
}
static void Pair(yaml_document_t *doc, int map, const char *key, const char *value) {
    int k = Scalar(doc, key), v = Scalar(doc, value);
    assert(k && v && yaml_document_append_mapping_pair(doc, map, k, v));
}
static int Slot(yaml_document_t *doc, int sequence, unsigned i, const char *replace, const char *value) {
    const char *keys[] = {"service_id", "core", "n3_port", "n6_port", "n3_vf", "n6_vf", "n3_ip", "n6_ip"};
    char values[8][32];
    snprintf(values[0], 32, "%u", i + 14);
    snprintf(values[1], 32, "%u", i + 3);
    snprintf(values[2], 32, "%u", 2 * i);
    snprintf(values[3], 32, "%u", 2 * i + 1);
    snprintf(values[4], 32, "%u", i);
    snprintf(values[5], 32, "%u", i);
    snprintf(values[6], 32, "192.168.2.%u", i + 11);
    snprintf(values[7], 32, "192.168.3.%u", i + 11);
    int slot = yaml_document_add_mapping(doc, NULL, YAML_BLOCK_MAPPING_STYLE);
    assert(slot && yaml_document_append_sequence_item(doc, sequence, slot));
    for (size_t k = 0; k < 8; k++) {
        int changed = replace && !strcmp(keys[k], replace);
        if (!changed || value) Pair(doc, slot, keys[k], changed ? value : values[k]);
    }
    return slot;
}
static int Config(yaml_document_t *doc, unsigned count, const char *slot_key, const char *slot_value) {
    assert(yaml_document_initialize(doc, NULL, NULL, NULL, 1, 1));
    int root = yaml_document_add_mapping(doc, NULL, YAML_BLOCK_MAPPING_STYLE);
    Pair(doc, root, "worker_binary", "/tmp/l25gc_upf_u");
    Pair(doc, root, "n3_pf", "ens1f0");
    Pair(doc, root, "n6_pf", "ens1f1");
    Pair(doc, root, "n3_peer_ip", "192.168.2.2");
    Pair(doc, root, "n6_peer_ip", "192.168.3.2");
    int key = Scalar(doc, "worker_slots");
    int seq = yaml_document_add_sequence(doc, NULL, YAML_BLOCK_SEQUENCE_STYLE);
    assert(yaml_document_append_mapping_pair(doc, root, key, seq));
    for (unsigned i = 0; i < count; i++) Slot(doc, seq, i, i == count - 1 ? slot_key : NULL, slot_value);
    return root;
}
static UpfScalingConfig Parse(yaml_document_t *doc, int root, int success) {
    UpfScalingConfig config, before;
    memset(&config, 0xa5, sizeof(config));
    before = config;
    char error[256];
    int rc = UpfScalingConfigParse(doc, yaml_document_get_node(doc, root), &config, error, sizeof(error));
    if (success && rc) fprintf(stderr, "%s\n", error);
    assert((rc == 0) == success);
    if (!success) { assert(error[0]); assert(!memcmp(&config, &before, sizeof(config))); }
    return config;
}

int main(void) {
    yaml_document_t doc;
    int root = Config(&doc, 2, NULL, NULL);
    UpfScalingConfig config = Parse(&doc, root, 1);
    assert(config.slot_count == 2 && config.min_workers == 1 && config.max_workers == 2);
    assert(config.rx_queue_threshold == 40 && config.teid_first == 0x1001 && config.teid_last == UINT32_MAX);
    assert(config.queue_sample_interval_ms == 10 && config.queue_window_samples == 10);
    assert(config.slots[0].service_id == 14 && config.slots[1].n6_port == 3);
    Pair(&doc, root, "queue_sample_interval_ms", "20");
    Pair(&doc, root, "queue_window_samples", "5");
    UpfScalingConfig custom = Parse(&doc, root, 1);
    assert(custom.queue_sample_interval_ms == 20 && custom.queue_window_samples == 5);
    Pair(&doc, root, "teid_first", "0xfffffffe");
    Pair(&doc, root, "teid_last", "4294967295");
    Parse(&doc, root, 1);
    Pair(&doc, root, "teid_first", "1"); /* Duplicate keys cannot silently override. */
    Parse(&doc, root, 0);
    yaml_document_delete(&doc);

    const char *bad_keys[] = {"min_workers", "max_workers", "max_workers", "rx_queue_threshold",
                             "rx_queue_threshold", "teid_first", "teid_last", "spawn_cooldown_ms",
                             "queue_sample_interval_ms", "queue_window_samples", "queue_window_samples",
                             "queue_window_samples", "queue_consecutive_samples"};
    const char *bad_values[] = {"0", "0", "3", "0", "-1", "4294967296", "4096", "1000", "0", "0",
                               "1025", "-1", "3"};
    for (size_t i = 0; i < sizeof(bad_keys) / sizeof(bad_keys[0]); i++) {
        root = Config(&doc, 2, NULL, NULL);
        Pair(&doc, root, bad_keys[i], bad_values[i]);
        Parse(&doc, root, 0);
        yaml_document_delete(&doc);
    }
    unsigned windows[] = {1, UPF_MAX_QUEUE_WINDOW_SAMPLES};
    for (unsigned i = 0; i < sizeof(windows) / sizeof(windows[0]); i++) {
        char value[16];
        snprintf(value, sizeof(value), "%u", windows[i]);
        root = Config(&doc, 2, NULL, NULL);
        Pair(&doc, root, "queue_window_samples", value);
        assert(Parse(&doc, root, 1).queue_window_samples == windows[i]);
        yaml_document_delete(&doc);
    }
    const char *slot_keys[] = {"service_id", "core", "n3_port", "n6_vf", "n3_ip", "n3_ip",
                              "n6_ip", "n3_ip", "n3_ip", "service_id", "core", "n3_vf", "n6_ip"};
    const char *slot_values[] = {"14", "3", "1", "0", "192.168.2.11", "192.168.3.11",
                                NULL, "999.1.1.1", "224.1.1.1", "0", "65536", "1junk", "0.0.0.0"};
    for (size_t i = 0; i < sizeof(slot_keys) / sizeof(slot_keys[0]); i++) {
        root = Config(&doc, 2, slot_keys[i], slot_values[i]);
        Parse(&doc, root, 0);
        yaml_document_delete(&doc);
    }
    unsigned counts[] = {0, UPF_MAX_WORKERS, UPF_MAX_WORKERS + 1};
    for (size_t i = 0; i < 3; i++) {
        root = Config(&doc, counts[i], NULL, NULL);
        Parse(&doc, root, counts[i] == UPF_MAX_WORKERS);
        yaml_document_delete(&doc);
    }

    /* Registry ownership, ABI checks, release/acquire publication and immutability. */
    assert(UpfWorkerRegistryAttach() == -ENOENT);
    process_type = RTE_PROC_SECONDARY;
    assert(UpfWorkerRegistryCreate(32) == -EPERM);
    process_type = RTE_PROC_PRIMARY;
    assert(UpfWorkerRegistryCreate(0) == -EINVAL);
    assert(UpfWorkerRegistryCreate(32) == 0);
    assert(!UpfWorkerRegistryIsConfigured() && g_upf_workers->service_limit == 32);
    assert(UpfWorkerRegistryCreate(32) == -EEXIST);
    process_type = RTE_PROC_SECONDARY;
    g_upf_workers = NULL;
    assert(UpfWorkerRegistryAttach() == 0);
    assert(UpfWorkerRegistryPublish(NULL) == -EINVAL);
    UpfScalingConfig invalid = config;
    invalid.slot_count = UPF_MAX_WORKERS + 1;
    assert(UpfWorkerRegistryPublish(&invalid) == -EINVAL && !UpfWorkerRegistryIsConfigured());
    assert(UpfWorkerRegistryPublish(&config) == 0 && UpfWorkerRegistryIsConfigured());
    for (unsigned i = 0; i < UPF_MAX_WORKERS; i++) {
        assert(g_upf_workers->runtime[i].state == UPF_WORKER_INACTIVE);
        assert(!g_upf_workers->runtime[i].instance_id && !g_upf_workers->runtime[i].generation);
    }
    config.slots[0].service_id = 20;
    assert(g_upf_workers->config.slots[0].service_id == 14); /* Value copy, not private pointers. */
    assert(UpfWorkerRegistryPublish(&config) == -EALREADY);
    zone.len--;
    assert(UpfWorkerRegistryAttach() == -EPROTO);
    zone.len++;
    storage.abi_version++;
    assert(UpfWorkerRegistryAttach() == -EPROTO);
    puts("PASS: worker-slot parsing, validation and shared registry");
    return 0;
}
