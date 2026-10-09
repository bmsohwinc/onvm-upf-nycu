#include "upf_sw_lb.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct upf_sw_lb lb;
static const char *base = "n3 0 10.10.2.11\nn6 1 10.10.3.11\n";
static const char *routes = "session 0x1001 10.60.0.1 14\nsession 0x1002 10.60.0.2 15\n";
static void w16(uint8_t *p, unsigned v) { p[0] = v >> 8; p[1] = v; }
static void w32(uint8_t *p, uint32_t v) { w16(p, v >> 16); w16(p + 2, v); }

static int load(const char *text) {
    char path[] = "/tmp/upf-lb-test-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "w");
    assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
    int result = upf_sw_lb_load(&lb, path);
    unlink(path);
    return result;
}

static void config(void) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s%s", base, routes);
    assert(load(buf) == 0);
    assert(lb.worker_count == 2 && lb.leader == 14 && lb.route_count == 2);
    assert(upf_sw_lb_lookup(&lb, 0x0a3c0001)->instance == 14);
    assert(!upf_sw_lb_lookup(&lb, 0x0a3c0003));
}

static size_t ip(uint8_t *p, uint32_t src, uint32_t dst, int proto, int options) {
    unsigned ihl = options ? 24 : 20, l4 = proto == 6 ? 20 : 8;
    memset(p, 0, ihl + l4);
    p[0] = 0x40 | ihl / 4; p[8] = 64; p[9] = proto;
    w16(p + 2, ihl + l4); w32(p + 12, src); w32(p + 16, dst);
    if (proto == 17) { w16(p + ihl, 1234); w16(p + ihl + 2, 5678); w16(p + ihl + 4, l4); }
    if (proto == 6) p[ihl + 12] = 0x50;
    if (proto == 1) p[ihl] = 8;
    return ihl + l4;
}

static size_t packet(uint8_t *p, int uplink, unsigned ue, int proto, int ext, int options) {
    memset(p, 0, 256); w16(p + 12, 0x0800);
    if (!uplink) return 14 + ip(p + 14, 0x0a0a0302, 0x0a3c0000 + ue, proto, options);
    unsigned ihl = options ? 24 : 20;
    ip(p + 14, 0x0a0a0201, 0x0a0a020b, 17, options);
    uint8_t *udp = p + 14 + ihl, *gtp = udp + 8;
    unsigned gtp_hdr = ext == 1 ? 20 : ext == 2 ? 12 : 8;
    gtp[0] = ext == 1 ? 0x34 : ext == 2 ? 0x32 : 0x30; gtp[1] = 255;
    w32(gtp + 4, 0x1000 + ue);
    if (ext == 1) {
        gtp[11] = 0x85; gtp[12] = 1; gtp[14] = 9; gtp[15] = 0x40;
        gtp[16] = 1; gtp[19] = 0;
    }
    size_t inner = ip(gtp + gtp_hdr, 0x0a3c0000 + ue, 0x0a0a0302, proto, options);
    w16(gtp + 2, gtp_hdr - 8 + inner);
    w16(udp + 2, 2152); w16(udp + 4, 8 + gtp_hdr + inner);
    w16(p + 16, ihl + 8 + gtp_hdr + inner);
    return 14 + ihl + 8 + gtp_hdr + inner;
}

int main(void) {
    config();
    uint8_t p[256], good[256];
    int protocols[] = {17, 6, 1};
    for (int ul = 0; ul <= 1; ul++)
        for (unsigned ue = 1; ue <= 2; ue++)
            for (unsigned proto = 0; proto < 3; proto++)
                for (int ext = 0; ext < 3; ext++)
                    for (int opt = 0; opt < 2; opt++) {
                        size_t n = packet(p, ul, ue, protocols[proto], ext, opt);
                        assert(upf_sw_lb_classify(&lb, ul ? 0 : 1, p, n) == 13 + (int)ue);
                        /* Each truncation is checked against a precisely-sized allocation. */
                        for (size_t cut = 0; cut < n; cut++) {
                            uint8_t *short_p = malloc(cut ? cut : 1);
                            memcpy(short_p, p, cut);
                            assert(upf_sw_lb_classify(&lb, ul ? 0 : 1, short_p, cut) == UPF_SW_LB_DROP);
                            free(short_p);
                        }
                    }
    size_t n = packet(good, 1, 1, 17, 1, 0);
    memcpy(p, good, n); w32(p + 46, 0x1002); /* another UE's TEID */
    assert(upf_sw_lb_classify(&lb, 0, p, n) == UPF_SW_LB_DROP);
    memcpy(p, good, n); p[54] = 0; /* zero extension length */
    assert(upf_sw_lb_classify(&lb, 0, p, n) == UPF_SW_LB_DROP);
    memcpy(p, good, n); p[54] = 255;
    assert(upf_sw_lb_classify(&lb, 0, p, n) == UPF_SW_LB_DROP);
    memcpy(p, good, n); p[42] = 0x20; /* wrong GTP protocol type */
    assert(upf_sw_lb_classify(&lb, 0, p, n) == UPF_SW_LB_DROP);
    memcpy(p, good, n); p[43] = 1; /* GTP Echo is not a T-PDU */
    assert(upf_sw_lb_classify(&lb, 0, p, n) == UPF_SW_LB_DROP);
    memcpy(p, good, n); p[20] = 0x20; /* outer fragment */
    assert(upf_sw_lb_classify(&lb, 0, p, n) == UPF_SW_LB_DROP);
    memcpy(p, good, n); p[14 + 20 + 8 + 20 + 6] = 0x20; /* inner fragment */
    assert(upf_sw_lb_classify(&lb, 0, p, n) == UPF_SW_LB_DROP);
    n = packet(p, 0, 3, 17, 0, 0);
    assert(upf_sw_lb_classify(&lb, 1, p, n) == UPF_SW_LB_DROP);
    assert(upf_sw_lb_classify(&lb, 9, NULL, 0) == 0);
    n = packet(p, 0, 1, 1, 0, 0); w32(p + 30, 0x0a0a030b);
    assert(upf_sw_lb_classify(&lb, 1, p, n) == 14); /* local ICMP */
    w16(p + 12, 0x0806);
    assert(upf_sw_lb_classify(&lb, 1, p, n) == UPF_SW_LB_ARP);
    w16(p + 12, 0x8100);
    assert(upf_sw_lb_classify(&lb, 1, p, n) == UPF_SW_LB_DROP);

    /* Deterministic malformed-packet fuzz, with allocator red zones. */
    uint32_t random = 7;
    for (unsigned trial = 0; trial < 20000; trial++) {
        size_t len = trial % sizeof(p);
        uint8_t *fuzz = malloc(len ? len : 1);
        for (size_t i = 0; i < len; i++) { random = random * 1664525 + 1013904223; fuzz[i] = random >> 24; }
        if (len > 14) { w16(fuzz + 12, 0x0800); fuzz[14] = 0x45; }
        upf_sw_lb_classify(&lb, trial & 1, fuzz, len);
        free(fuzz);
    }
    assert(!upf_sw_lb_all_seen(&lb, 2));
    lb.state[14].version = 2;
    assert(!upf_sw_lb_all_seen(&lb, 2));
    lb.state[15].version = 4;
    assert(upf_sw_lb_all_seen(&lb, 2));
    assert(!upf_sw_lb_all_seen(&lb, 4) && !upf_sw_lb_all_seen(&lb, 3));

    const char *bad[] = {
        "session 0x1001 10.60.0.2 15\n", /* duplicate TEID */
        "session 0x1002 10.60.0.1 15\n", /* duplicate UE */
        "session 0 10.60.0.2 15\n", "session 0x1002 10.60.0.2 128\n",
        "session 0x1002 10.60.0.2 0\n", "session -1 10.60.0.2 15\n",
        "session 4294967296 10.60.0.2 15\n", "session 2 10.60.0.999 15\n",
        "session 2 10.60.0.2 15 extra\n", "session 2 10.10.2.11 15\n",
        "n3 4 10.10.2.11\n", "unknown 1 2\n"
    };
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%ssession 0x1001 10.60.0.1 14\n%s", base, bad[i]);
        assert(load(buf) < 0);
    }
    assert(load(base) < 0);
    assert(load(routes) < 0);
    assert(load("n3 0 10.10.2.11\nn6 0 10.10.3.11\nsession 1 10.60.0.1 14\n") < 0);
    puts("parser/config/affinity/truncation/fuzz/ACK barrier: PASS");
    return 0;
}
