/* Compile the production port configuration and init_port with mocked ethdev. */
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RTE_ETH_NAME_MAX_LEN 64
#define RTE_ETHER_MTU 1500
#define RTE_ETH_MQ_RX_NONE 0
#define RTE_ETH_MQ_RX_RSS 1
#define RTE_ETH_MQ_TX_NONE 0
#define RTE_ETH_RX_OFFLOAD_CHECKSUM 7u
#define RTE_ETH_TX_OFFLOAD_IPV4_CKSUM 1u
#define RTE_ETH_TX_OFFLOAD_UDP_CKSUM 2u
#define RTE_ETH_TX_OFFLOAD_TCP_CKSUM 4u
#define RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE 8u
#define RTE_ETH_RSS_IP 1u
#define RTE_ETH_RSS_UDP 2u
#define RTE_ETH_RSS_TCP 4u
#define RTE_ETH_RSS_L2_PAYLOAD 8u
#define RTE_MP_RX_DESC_DEFAULT 1024
#define RTE_MP_TX_DESC_DEFAULT 1024
#define ONVM_NUM_MGR_AUX_THREADS 1
#define MAX_MTU 9600
static unsigned rx_threads = 1, tx_threads = 1;
#define ONVM_NUM_RX_THREADS rx_threads
static int ONVM_USE_JUMBO_FRAMES;
static uint8_t rss_symmetric_key[40];
struct rte_eth_rxconf { uint64_t offloads; };
struct rte_eth_txconf { uint64_t offloads; };
struct rte_eth_conf {
    struct { int mq_mode; uint32_t mtu; uint64_t offloads; } rxmode;
    struct { struct { uint8_t *rss_key; uint64_t rss_hf; uint8_t rss_key_len; } rss_conf; } rx_adv_conf;
    struct { int mq_mode; uint64_t offloads; } txmode;
};
struct rte_eth_dev_info {
    uint16_t max_rx_queues, max_tx_queues;
    uint64_t rx_offload_capa, tx_offload_capa, flow_type_rss_offloads;
    uint8_t hash_key_size;
    const char *driver_name;
    struct rte_eth_rxconf default_rxconf;
    struct rte_eth_txconf default_txconf;
};
static struct { int init[2]; } port_state, *ports = &port_state;
static void *pktmbuf_pool;
static struct rte_eth_dev_info device;
static struct rte_eth_conf configured;
static int info_error, rx_error, start_error, promisc_error;
static unsigned configured_count, rx_queues, tx_queues, starts;

static unsigned rte_lcore_count(void) { return rx_threads + tx_threads + ONVM_NUM_MGR_AUX_THREADS; }
static unsigned rte_eth_dev_socket_id(unsigned port) { (void)port; return 0; }
static int rte_eth_dev_get_name_by_port(unsigned port, char *name) {
    snprintf(name, RTE_ETH_NAME_MAX_LEN, "0000:06:10.%u", port);
    return 0;
}
static int rte_eth_dev_info_get(unsigned port, struct rte_eth_dev_info *info) {
    (void)port;
    if (info_error) return info_error;
    *info = device;
    return 0;
}
static int rte_eth_dev_configure(unsigned port, unsigned rx, unsigned tx, const struct rte_eth_conf *conf) {
    assert(port < 2 && rx == rx_threads && tx == tx_threads);
    assert(!(conf->rxmode.offloads & ~device.rx_offload_capa));
    assert(!(conf->txmode.offloads & ~device.tx_offload_capa));
    configured = *conf;
    configured_count++;
    return 0;
}
static int rte_eth_dev_adjust_nb_rx_tx_desc(unsigned port, uint16_t *rx, uint16_t *tx) {
    (void)port; *rx = *tx = 512; return 0;
}
#define rte_panic(...) abort()
static int rte_eth_rx_queue_setup(unsigned port, unsigned queue, unsigned count, unsigned socket,
                                  const struct rte_eth_rxconf *conf, void *pool) {
    (void)port; (void)socket; (void)pool;
    assert(queue == rx_queues++ && count == 512);
    assert(conf->offloads == configured.rxmode.offloads);
    return rx_error;
}
static int rte_eth_tx_queue_setup(unsigned port, unsigned queue, unsigned count, unsigned socket,
                                  const struct rte_eth_txconf *conf) {
    (void)port; (void)socket;
    assert(queue == tx_queues++ && count == 512);
    assert(conf->offloads == configured.txmode.offloads);
    return 0;
}
static int rte_eth_promiscuous_enable(unsigned port) { (void)port; return promisc_error; }
static int rte_eth_dev_start(unsigned port) {
    (void)port;
    assert(rx_queues == rx_threads && tx_queues == tx_threads);
    starts++;
    return start_error;
}

/* SOURCE_UNDER_TEST */

static void reset(void) {
    rx_threads = tx_threads = 1;
    ONVM_USE_JUMBO_FRAMES = 0;
    device = (struct rte_eth_dev_info){
        .max_rx_queues = 4, .max_tx_queues = 4, .rx_offload_capa = 3, .tx_offload_capa = 10,
        .flow_type_rss_offloads = 7, .hash_key_size = 40, .driver_name = "net_ixgbe_vf",
    };
    info_error = rx_error = start_error = promisc_error = 0;
    configured_count = rx_queues = tx_queues = starts = 0;
    memset(&port_state, 0, sizeof(port_state));
}

int main(void) {
    reset();
    assert(init_port(1) == 0 && ports->init[1] && starts == 1);
    assert(configured.rxmode.mq_mode == RTE_ETH_MQ_RX_NONE);
    assert(configured.rxmode.mtu == 1500 && configured.rxmode.offloads == 3);
    assert(configured.txmode.offloads == 10); /* Includes optional fast-free at port and queue. */
    assert(!configured.rx_adv_conf.rss_conf.rss_key && !configured.rx_adv_conf.rss_conf.rss_hf &&
           !configured.rx_adv_conf.rss_conf.rss_key_len);

    reset();
    rx_threads = 2;
    assert(init_port(0) == 0 && configured.rxmode.mq_mode == RTE_ETH_MQ_RX_RSS);
    assert(configured.rx_adv_conf.rss_conf.rss_hf == 7 && configured.rx_adv_conf.rss_conf.rss_key_len == 40);

    reset();
    rx_threads = 2;
    device.flow_type_rss_offloads = 0;
    device.rx_offload_capa = device.tx_offload_capa = 0;
    ONVM_USE_JUMBO_FRAMES = 1;
    assert(init_port(0) == 0 && configured.rxmode.mq_mode == RTE_ETH_MQ_RX_NONE);
    assert(configured.rxmode.mtu == MAX_MTU);
    assert(!configured.rxmode.offloads && !configured.txmode.offloads);

    reset();
    device.max_rx_queues = 0;
    assert(init_port(1) == -EINVAL && !configured_count && !ports->init[1]);
    reset();
    device.max_tx_queues = 0;
    assert(init_port(1) == -EINVAL && !configured_count && !ports->init[1]);
    reset();
    info_error = -EIO;
    assert(init_port(1) == -EIO && !configured_count && !ports->init[1]);
    reset();
    rx_error = -ENOMEM;
    assert(init_port(1) == -ENOMEM && !starts && !ports->init[1]);
    reset();
    start_error = -EIO;
    assert(init_port(1) == -EIO && !ports->init[1]);
    reset();
    promisc_error = -ENOTSUP; /* Failure is visible but unicast can still work. */
    assert(init_port(1) == 0 && ports->init[1]);
    puts("port init: single-queue RSS disabled/capabilities/queue offloads/error handling: PASS");
}
