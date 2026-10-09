#include "upf_sw_lb.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t
hash_key(uint32_t key) {
    key ^= key >> 16;
    key *= 0x7feb352du;
    key ^= key >> 15;
    return key & (UPF_SW_LB_CAPACITY - 1);
}

const struct upf_sw_lb_route *
upf_sw_lb_lookup(const struct upf_sw_lb *lb, uint32_t ip) {
    uint32_t slot = hash_key(ip);
    for (unsigned n = 0; n < UPF_SW_LB_CAPACITY; n++) {
        const struct upf_sw_lb_route *r = &lb->routes[slot];
        if (!r->instance) return NULL;
        if (r->ue_ip == ip) return r;
        slot = (slot + 1) & (UPF_SW_LB_CAPACITY - 1);
    }
    return NULL;
}

const struct upf_sw_lb_route *
upf_sw_lb_lookup_teid(const struct upf_sw_lb *lb, uint32_t teid) {
    uint32_t slot = hash_key(teid);
    for (unsigned n = 0; n < UPF_SW_LB_CAPACITY; n++) {
        const struct upf_sw_lb_route *r = &lb->teid_routes[slot];
        if (!r->instance) return NULL;
        if (r->teid == teid) return r;
        slot = (slot + 1) & (UPF_SW_LB_CAPACITY - 1);
    }
    return NULL;
}

static int
number(const char *s, uint32_t max, uint32_t *value) {
    char *end;
    if (!*s || *s == '-' || *s == '+') return -1;
    errno = 0;
    unsigned long v = strtoul(s, &end, strncmp(s, "0x", 2) == 0 ? 16 : 10);
    if (errno || *end || v > max) return -1;
    *value = (uint32_t)v;
    return 0;
}

int
upf_sw_lb_load(struct upf_sw_lb *lb, const char *path) {
    FILE *f = fopen(path, "r");
    char line[256], kind[32], a[64], b[64], c[64], extra[2];
    unsigned lineno = 0, have_n3 = 0, have_n6 = 0;
    if (!f) { perror(path); return -1; }
    memset(lb, 0, sizeof(*lb));
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        if (!strchr(line, '\n') && !feof(f)) goto bad;
        char *comment = strchr(line, '#');
        if (comment) *comment = '\0';
        int fields = sscanf(line, "%31s %63s %63s %63s %1s", kind, a, b, c, extra);
        if (fields <= 0) continue;
        uint32_t value, instance;
        struct in_addr ip;
        if (fields == 3 && (!strcmp(kind, "n3") || !strcmp(kind, "n6"))) {
            if (number(a, UINT16_MAX, &value) || inet_pton(AF_INET, b, &ip) != 1 || !ip.s_addr)
                goto bad;
            if (!strcmp(kind, "n3")) {
                if (have_n3++) goto bad;
                lb->n3_port = value; lb->n3_ip = ntohl(ip.s_addr);
            } else {
                if (have_n6++) goto bad;
                lb->n6_port = value; lb->n6_ip = ntohl(ip.s_addr);
            }
        } else if (fields == 4 && !strcmp(kind, "session")) {
            if (number(a, UINT32_MAX, &value) || !value ||
                inet_pton(AF_INET, b, &ip) != 1 || !ip.s_addr ||
                number(c, UPF_SW_LB_WORKERS - 1, &instance) || !instance ||
                lb->route_count == UPF_SW_LB_CAPACITY / 2)
                goto bad;
            uint32_t ue_ip = ntohl(ip.s_addr);
            if (upf_sw_lb_lookup(lb, ue_ip) || upf_sw_lb_lookup_teid(lb, value)) goto bad;
            uint32_t slot = hash_key(ue_ip);
            while (lb->routes[slot].instance)
                slot = (slot + 1) & (UPF_SW_LB_CAPACITY - 1);
            lb->routes[slot] = (struct upf_sw_lb_route){ue_ip, value, instance};
            slot = hash_key(value);
            while (lb->teid_routes[slot].instance)
                slot = (slot + 1) & (UPF_SW_LB_CAPACITY - 1);
            lb->teid_routes[slot] = (struct upf_sw_lb_route){ue_ip, value, instance};
            lb->route_count++;
            if (!lb->workers[instance]) { lb->workers[instance] = 1; lb->worker_count++; }
            if (!lb->leader || instance < lb->leader) lb->leader = instance;
        } else goto bad;
    }
    if (ferror(f) || !have_n3 || !have_n6 || lb->n3_port == lb->n6_port ||
        !lb->route_count || lb->worker_count > 32) goto bad;
    for (unsigned i = 0; i < UPF_SW_LB_CAPACITY; i++)
        if (lb->routes[i].instance &&
            (lb->routes[i].ue_ip == lb->n3_ip || lb->routes[i].ue_ip == lb->n6_ip)) goto bad;
    fclose(f);
    return 0;
bad:
    fprintf(stderr, "%s:%u: invalid UPF LB configuration (syntax, duplicate, or limit)\n", path, lineno);
    fclose(f);
    return -1;
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)be16(p) << 16 | be16(p + 2); }

/* This baseline matches the UPF's untagged IPv4, contiguous-mbuf path.
 * Reject fragments: L4/PDR parsing in the worker does not reassemble them. */
static int
ipv4(const uint8_t *p, size_t available, size_t *hlen, size_t *total) {
    if (available < 20 || p[0] >> 4 != 4) return -1;
    *hlen = (p[0] & 15) * 4;
    *total = be16(p + 2);
    return *hlen < 20 || *total < *hlen || *total > available || (be16(p + 6) & 0x3fff) ? -1 : 0;
}

static int
l4_valid(const uint8_t *p, size_t hlen, size_t total) {
    size_t payload = total - hlen;
    if (p[9] == 17)
        return payload >= 8 && be16(p + hlen + 4) >= 8 && be16(p + hlen + 4) == payload;
    if (p[9] == 6)
        return payload >= 20 && (p[hlen + 12] >> 4) >= 5 && (size_t)(p[hlen + 12] >> 4) * 4 <= payload;
    if (p[9] == 1) return payload >= 8;
    return 0;
}

int
upf_sw_lb_classify(const struct upf_sw_lb *lb, uint16_t port,
                   const uint8_t *p, size_t length) {
    if (port != lb->n3_port && port != lb->n6_port) return 0;
    if (length < 14) return UPF_SW_LB_DROP;
    if (be16(p + 12) == 0x0806) return length >= 42 ? UPF_SW_LB_ARP : UPF_SW_LB_DROP;
    if (be16(p + 12) != 0x0800) return UPF_SW_LB_DROP;
    const uint8_t *ip = p + 14;
    size_t ihl, total;
    if (ipv4(ip, length - 14, &ihl, &total) || !l4_valid(ip, ihl, total)) return UPF_SW_LB_DROP;
    uint32_t dst = be32(ip + 16);
    if (ip[9] == 1 && (dst == lb->n3_ip || dst == lb->n6_ip)) return lb->leader;
    if (port == lb->n6_port) {
        const struct upf_sw_lb_route *r = upf_sw_lb_lookup(lb, dst);
        return r ? r->instance : UPF_SW_LB_DROP;
    }
    if (dst != lb->n3_ip || ip[9] != 17) return UPF_SW_LB_DROP;
    const uint8_t *udp = ip + ihl;
    if (be16(udp + 2) != 2152 || total - ihl < 16) return UPF_SW_LB_DROP;
    const uint8_t *gtp = udp + 8;
    size_t gtp_len = 8 + be16(gtp + 2);
    /* GTPv1-U T-PDU, reserved bit clear, complete base/optional header.
     * TEID is in the fixed header: leave extensions and inner parsing to UPF-U. */
    if ((gtp[0] & 0xf8) != 0x30 || gtp[1] != 255 || gtp_len != total - ihl - 8)
        return UPF_SW_LB_DROP;
    if ((gtp[0] & 7) && gtp_len < 12) return UPF_SW_LB_DROP;
    const struct upf_sw_lb_route *r = upf_sw_lb_lookup_teid(lb, be32(gtp + 4));
    return r ? r->instance : UPF_SW_LB_DROP;
}

int
upf_sw_lb_all_seen(const struct upf_sw_lb *lb, uint32_t version) {
    if (!version || (version & 1)) return 0;
    for (unsigned i = 1; i < UPF_SW_LB_WORKERS; i++)
        if (lb->workers[i] && __atomic_load_n(&lb->state[i].version, __ATOMIC_ACQUIRE) < version)
            return 0;
    return 1;
}
