/* SPDX-License-Identifier: Apache-2.0 */
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "upf_worker.h"
#ifdef __linux__
#include <linux/ethtool.h>
#else
/* Fields used by the production ioctl builder, for running on macOS. */
#define ETHTOOL_SRXCLSRLINS 1
#define ETHTOOL_SRXCLSRLDEL 2
#define ETHTOOL_GRXCLSRULE 3
#define ETHTOOL_RX_FLOW_SPEC_RING_VF_OFF 32
#define IP_USER_FLOW 13
#define UDP_V4_FLOW 2
#define ETH_RX_NFC_IP4 1
#define FLOW_EXT (1u << 31)
union flow_header {
    struct { uint32_t ip4src, ip4dst; uint16_t psrc, pdst; uint8_t tos; } tcp_ip4_spec;
    struct { uint32_t ip4src, ip4dst, l4_4_bytes; uint8_t tos, ip_ver, proto; } usr_ip4_spec;
    unsigned char data[52];
};
struct flow_ext { uint8_t padding[2], h_dest[6]; uint16_t vlan_etype, vlan_tci; uint32_t data[2]; };
struct ethtool_rx_flow_spec {
    uint32_t flow_type;
    union flow_header h_u;
    struct flow_ext h_ext;
    union flow_header m_u;
    struct flow_ext m_ext;
    uint64_t ring_cookie;
    uint32_t location;
};
struct ethtool_rxnfc { uint32_t cmd; struct ethtool_rx_flow_spec fs; };
#endif
static UpfWorkerRegistry registry;
UpfWorkerRegistry *g_upf_workers = &registry;
static uint32_t capacity[] = {32, 1024};
static struct ethtool_rx_flow_spec stored;
static int present, reads, writes, read_error, write_error;
static int ethctl(const char *pf, void *data) {
    (void)pf;
    struct ethtool_rxnfc *cmd = data;
    if (cmd->cmd == ETHTOOL_GRXCLSRULE) {
        reads++;
        if (read_error) return read_error;
        if (!present) return -ENOENT;
        cmd->fs = stored; cmd->fs.flow_type |= FLOW_EXT; /* ixgbe readback. */
        return 0;
    }
    writes++;
    if (write_error) return write_error;
    present = cmd->cmd == ETHTOOL_SRXCLSRLINS;
    if (present) stored = cmd->fs;
    return 0;
}
#include "filter.inc"

int main(void) {
    struct in_addr ip = {.s_addr = htonl(0x0a3c0001)};
    for (unsigned side = 0; side < 2; side++) {
        assert(!filter(side, 0, 1, ip, 1));
        assert(stored.ring_cookie == (UINT64_C(2) << 32));
        assert(filter(side, 0, 1, ip, 1) == -EEXIST);
        assert(filter(side, 0, 2, ip, 0) == -ESTALE);
        stored.h_u.tcp_ip4_spec.ip4dst++;
        assert(filter(side, 0, 1, ip, 0) == -ESTALE);
        stored.h_u.tcp_ip4_spec.ip4dst--;
        stored.m_ext.vlan_tci = 1;
        assert(filter(side, 0, 1, ip, 0) == -ESTALE);
        stored.m_ext.vlan_tci = 0;
        write_error = -EIO;
        assert(filter(side, 0, 1, ip, 0) == -EIO && present);
        write_error = 0;
        assert(!filter(side, 0, 1, ip, 0) && !present);
        int old_writes = writes;
        assert(!filter(side, 0, 1, ip, 0) && writes == old_writes);
    }
    read_error = -EPERM;
    assert(filter(0, 0, 1, ip, 0) == -EPERM);
    int old_reads = reads;
    assert(filter(0, 32, 1, ip, 0) == -ENOSPC && reads == old_reads);
    puts("PASS: ixgbe readback normalization, filter ownership and ioctl errors");
}
