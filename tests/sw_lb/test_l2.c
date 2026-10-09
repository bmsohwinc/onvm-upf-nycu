/* Built together with the actual MAC parser/cache/attach functions. */
#include <assert.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
struct rte_ether_addr { uint8_t addr_bytes[6]; };
struct rte_ether_hdr { struct rte_ether_addr dst_addr, src_addr; uint16_t ether_type; };
struct rte_mbuf { struct rte_ether_hdr eth; };
struct onvm_nf { int unused; };
struct neigh_entry { struct rte_ether_addr mac; int state; };
#define NEIGH_REACHABLE 1
#define RTE_ETHER_TYPE_IPV4 0x0800
#define rte_cpu_to_be_16 htons
static uint16_t g_n3_port = 0, g_n6_port = 1;
static struct rte_ether_addr g_cn_ue_eth = {{2, 0, 0, 0, 0, 1}};
static struct rte_ether_addr g_cn_dn_eth = {{2, 0, 0, 0, 0, 2}};
static struct rte_ether_addr g_n3_peer_mac = {{2, 0, 0, 0, 1, 1}};
static struct rte_ether_addr g_n6_peer_mac = {{2, 0, 0, 0, 1, 2}};
static uint8_t g_n3_peer_mac_set, g_n6_peer_mac_set;
static unsigned lookup_count, arp_count;
static int have_neighbor, no_headroom;
static struct neigh_entry neighbor = {{{2, 0, 0, 0, 2, 1}}, NEIGH_REACHABLE};
static struct neigh_entry *neigh_lookup(uint16_t port, uint32_t ip) {
    (void)port; (void)ip; lookup_count++; return have_neighbor ? &neighbor : NULL;
}
static int send_arp_request(uint16_t port, uint32_t src, uint32_t dst, struct onvm_nf *nf) {
    (void)port; (void)src; (void)dst; (void)nf; arp_count++; return 0;
}
static void *rte_pktmbuf_prepend(struct rte_mbuf *p, size_t size) {
    assert(size == sizeof(p->eth)); return no_headroom ? NULL : &p->eth;
}
static void rte_ether_addr_copy(const struct rte_ether_addr *src, struct rte_ether_addr *dst) { *dst = *src; }

/* SOURCE_UNDER_TEST */

int main(void) {
    uint8_t bytes[6];
    assert(parse_mac("02:ab:cd:ef:01:02", bytes) == 0 && bytes[1] == 0xab);
    const char *bad[] = {"", "00:00:00:00:00:00", "ff:ff:ff:ff:ff:ff", "01:00:00:00:00:01",
                        "100:00:00:00:00:01", "02:00:00:00:01", "02:00:00:00:00:01junk"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) assert(parse_mac(bad[i], bytes) < 0);
    struct rte_mbuf p;
    struct onvm_nf nf;
    assert(attach_l2_or_arp(&p, 0, 0, 0, &nf) < 0 && arp_count == 1);
    have_neighbor = 1;
    assert(attach_l2_or_arp(&p, 0, 0, 0, &nf) == 0);
    assert(!memcmp(&p.eth.src_addr, &g_cn_ue_eth, 6) && !memcmp(&p.eth.dst_addr, &neighbor.mac, 6));
    assert(p.eth.ether_type == htons(0x0800));
    g_n3_peer_mac_set = g_n6_peer_mac_set = 1;
    unsigned before = lookup_count;
    assert(attach_l2_or_arp(&p, 0, 0, 0, &nf) == 0);
    assert(!memcmp(&p.eth.dst_addr, &g_n3_peer_mac, 6));
    assert(attach_l2_or_arp(&p, 1, 0, 0, &nf) == 0);
    assert(!memcmp(&p.eth.src_addr, &g_cn_dn_eth, 6) && !memcmp(&p.eth.dst_addr, &g_n6_peer_mac, 6));
    assert(lookup_count == before); /* fixed peer path bypasses ARP lookup */
    no_headroom = 1; assert(attach_l2_or_arp(&p, 1, 0, 0, &nf) < 0);
    no_headroom = 0; assert(attach_l2_or_arp(&p, 9, 0, 0, &nf) < 0);
    puts("MAC validation/source cache/static peers/ARP fallback: PASS");
    return 0;
}
