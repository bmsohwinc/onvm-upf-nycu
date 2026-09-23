/* SPDX-License-Identifier: Apache-2.0 */
#include "upf_worker_config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { FIELD_U16, FIELD_U32, FIELD_IP, FIELD_STRING, FIELD_SLOTS } FieldType;
typedef struct {
    const char *name;
    FieldType type;
    size_t offset;
    size_t size;
    int required;
} Field;

#define FIELD(struct_type, member, type, required) \
    {#member, type, offsetof(struct_type, member), sizeof(((struct_type *)0)->member), required}

static const Field slot_fields[] = {
    FIELD(UpfWorkerSlotConfig, service_id, FIELD_U16, 1),
    FIELD(UpfWorkerSlotConfig, core, FIELD_U16, 1),
    FIELD(UpfWorkerSlotConfig, n3_port, FIELD_U16, 1),
    FIELD(UpfWorkerSlotConfig, n6_port, FIELD_U16, 1),
    FIELD(UpfWorkerSlotConfig, n3_vf, FIELD_U16, 1),
    FIELD(UpfWorkerSlotConfig, n6_vf, FIELD_U16, 1),
    {"n3_ip", FIELD_IP, offsetof(UpfWorkerSlotConfig, n3_addr), sizeof(struct in_addr), 1},
    {"n6_ip", FIELD_IP, offsetof(UpfWorkerSlotConfig, n6_addr), sizeof(struct in_addr), 1},
};

static const Field scaling_fields[] = {
    FIELD(UpfScalingConfig, min_workers, FIELD_U16, 0),
    FIELD(UpfScalingConfig, max_workers, FIELD_U16, 0),
    FIELD(UpfScalingConfig, rx_queue_threshold, FIELD_U32, 0),
    FIELD(UpfScalingConfig, teid_first, FIELD_U32, 0),
    FIELD(UpfScalingConfig, teid_last, FIELD_U32, 0),
    FIELD(UpfScalingConfig, worker_binary, FIELD_STRING, 1),
    FIELD(UpfScalingConfig, n3_pf, FIELD_STRING, 1),
    FIELD(UpfScalingConfig, n6_pf, FIELD_STRING, 1),
    FIELD(UpfScalingConfig, file_prefix, FIELD_STRING, 0),
    FIELD(UpfScalingConfig, dn_route_helper, FIELD_STRING, 1),
    FIELD(UpfScalingConfig, dn_host, FIELD_STRING, 1),
    FIELD(UpfScalingConfig, dn_interface, FIELD_STRING, 1),
    FIELD(UpfScalingConfig, startup_timeout_ms, FIELD_U32, 0),
    {"n3_peer_ip", FIELD_IP, offsetof(UpfScalingConfig, n3_peer_addr), sizeof(struct in_addr), 1},
    {"n6_peer_ip", FIELD_IP, offsetof(UpfScalingConfig, n6_peer_addr), sizeof(struct in_addr), 1},
    {"worker_slots", FIELD_SLOTS, 0, 0, 1},
};

static int Fail(char *error, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    if (error && size) vsnprintf(error, size, format, args);
    va_end(args);
    return -1;
}

static const char *Scalar(yaml_node_t *node) {
    if (!node || node->type != YAML_SCALAR_NODE ||
        strlen((const char *)node->data.scalar.value) != node->data.scalar.length)
        return NULL;
    return (const char *)node->data.scalar.value;
}

static int ParseFields(yaml_document_t *doc, yaml_node_t *node, void *out,
                       const Field *fields, size_t count, char *error, size_t error_size) {
    if (!node || node->type != YAML_MAPPING_NODE)
        return Fail(error, error_size, "Expected a mapping");
    unsigned seen = 0;
    for (yaml_node_pair_t *pair = node->data.mapping.pairs.start;
         pair < node->data.mapping.pairs.top; pair++) {
        const char *key = Scalar(yaml_document_get_node(doc, pair->key));
        yaml_node_t *value = yaml_document_get_node(doc, pair->value);
        size_t i;
        for (i = 0; key && i < count; i++)
            if (!strcmp(key, fields[i].name)) break;
        if (!key || i == count) return Fail(error, error_size, "Unknown key: %s", key ? key : "<non-scalar>");
        if (seen & (1u << i)) return Fail(error, error_size, "Duplicate key: %s", key);
        seen |= 1u << i;
        const Field *field = &fields[i];
        void *destination = (char *)out + field->offset;
        if (field->type == FIELD_SLOTS) {
            if (!value || value->type != YAML_SEQUENCE_NODE)
                return Fail(error, error_size, "worker_slots must be a nonempty sequence");
            UpfScalingConfig *config = out;
            for (yaml_node_item_t *item = value->data.sequence.items.start;
                 item < value->data.sequence.items.top; item++) {
                if (config->slot_count == UPF_MAX_WORKERS)
                    return Fail(error, error_size, "Too many worker slots (maximum %u)", UPF_MAX_WORKERS);
                if (ParseFields(doc, yaml_document_get_node(doc, *item),
                                &config->slots[config->slot_count], slot_fields,
                                sizeof(slot_fields) / sizeof(slot_fields[0]), error, error_size) < 0)
                    return -1;
                config->slot_count++;
            }
            if (!config->slot_count) return Fail(error, error_size, "worker_slots must not be empty");
            continue;
        }
        const char *text = Scalar(value);
        if (!text || !*text) return Fail(error, error_size, "%s must be a nonempty scalar", key);
        if (field->type == FIELD_STRING) {
            if (strlen(text) >= field->size) return Fail(error, error_size, "%s is too long", key);
            strcpy(destination, text);
        } else if (field->type == FIELD_IP) {
            struct in_addr ip;
            if (inet_pton(AF_INET, text, &ip) != 1 || !ip.s_addr ||
                ip.s_addr == INADDR_BROADCAST || IN_MULTICAST(ntohl(ip.s_addr)))
                return Fail(error, error_size, "%s must be a unicast IPv4 address", key);
            memcpy(destination, &ip, sizeof(ip));
        } else {
            if (text[0] < '0' || text[0] > '9') return Fail(error, error_size, "Invalid integer for %s", key);
            int base = !strncmp(text, "0x", 2) || !strncmp(text, "0X", 2) ? 16 : 10;
            char *end;
            errno = 0;
            unsigned long long n = strtoull(text, &end, base);
            if (errno || *end || n > (field->type == FIELD_U16 ? UINT16_MAX : UINT32_MAX))
                return Fail(error, error_size, "Invalid integer for %s", key);
            if (field->type == FIELD_U16) *(uint16_t *)destination = (uint16_t)n;
            else *(uint32_t *)destination = (uint32_t)n;
        }
    }
    for (size_t i = 0; i < count; i++)
        if (fields[i].required && !(seen & (1u << i)))
            return Fail(error, error_size, "Missing %s", fields[i].name);
    /* An omitted maximum means the full pool, but explicit zero is invalid. */
    if (fields == scaling_fields && !(seen & (1u << 1)))
        ((UpfScalingConfig *)out)->max_workers = ((UpfScalingConfig *)out)->slot_count;
    return 0;
}

int UpfScalingConfigParse(yaml_document_t *doc, yaml_node_t *mapping,
                          UpfScalingConfig *out, char *error, size_t error_size) {
    if (!doc || !out) return Fail(error, error_size, "Missing document or output");
    UpfScalingConfig config = {
        .min_workers = 1, .rx_queue_threshold = 1024,
        .teid_first = 0x1001, .teid_last = UINT32_MAX,
        .file_prefix = "rte", .startup_timeout_ms = 30000,
    };
    if (ParseFields(doc, mapping, &config, scaling_fields,
                    sizeof(scaling_fields) / sizeof(scaling_fields[0]), error, error_size) < 0)
        return -1;
    if (!config.min_workers || config.min_workers > config.max_workers || config.max_workers > config.slot_count)
        return Fail(error, error_size, "Require 1 <= min_workers <= max_workers <= slot count");
    if (!config.rx_queue_threshold || !config.teid_first || config.teid_first > config.teid_last)
        return Fail(error, error_size, "Queue threshold and TEID range must be positive and ordered");
    if (config.worker_binary[0] != '/' || config.dn_route_helper[0] != '/')
        return Fail(error, error_size, "worker_binary and dn_route_helper must be absolute paths");
    if (config.startup_timeout_ms < 1000 || config.startup_timeout_ms > 300000)
        return Fail(error, error_size, "startup_timeout_ms must be between 1000 and 300000");
    if (strspn(config.file_prefix, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != strlen(config.file_prefix))
        return Fail(error, error_size, "file_prefix must contain only letters, digits, underscore or hyphen");
    if (!strcmp(config.n3_pf, config.n6_pf))
        return Fail(error, error_size, "N3 and N6 must name different PFs");
    for (uint16_t i = 0; i < config.slot_count; i++) {
        const UpfWorkerSlotConfig *a = &config.slots[i];
        if (!a->service_id || a->n3_port == a->n6_port || a->n3_addr.s_addr == a->n6_addr.s_addr)
            return Fail(error, error_size, "Slot %u requires a nonzero service and distinct ports/IPs", i);
        for (uint16_t j = 0; j < i; j++) {
            const UpfWorkerSlotConfig *b = &config.slots[j];
            if (a->service_id == b->service_id || a->core == b->core ||
                a->n3_port == b->n3_port || a->n3_port == b->n6_port ||
                a->n6_port == b->n3_port || a->n6_port == b->n6_port ||
                a->n3_vf == b->n3_vf || a->n6_vf == b->n6_vf ||
                a->n3_addr.s_addr == b->n3_addr.s_addr || a->n3_addr.s_addr == b->n6_addr.s_addr ||
                a->n6_addr.s_addr == b->n3_addr.s_addr || a->n6_addr.s_addr == b->n6_addr.s_addr)
                return Fail(error, error_size, "Slots %u and %u share a service, core, port, VF or IP", j, i);
        }
    }
    *out = config;
    if (error && error_size) error[0] = '\0';
    return 0;
}
