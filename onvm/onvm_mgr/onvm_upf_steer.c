/* SPDX-License-Identifier: Apache-2.0
 * Linux ixgbe PF control. Devices, VF membership and ntuple mode are prepared
 * before manager startup; this module only adds/removes owned flow entries.
 */
#include <errno.h>
#include <arpa/inet.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>

#include "onvm_mgr.h"
#include "onvm_upf.h"
#include "upf_worker.h"

extern char **environ;
static int control_fd = -1, probed;
static uint32_t capacity[2];
static uint32_t n3_generation[UPF_MAX_WORKERS];
static struct {
    uint32_t generation;
    uint16_t slot;
    struct in_addr ue;
    int uncertain_route;
} sessions[UPF_MAX_SESSION_RULES];
static UpfSteerUpdate pending;
static uint32_t pending_sequence;
static pid_t helper_pid;
static uint64_t helper_deadline;
static int helper_timed_out;

static int ethctl(const char *pf, void *command) {
    struct ifreq ifr = {0};
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", pf);
    ifr.ifr_data = command;
    return ioctl(control_fd, SIOCETHTOOL, &ifr) < 0 ? -errno : 0;
}

static int probe_pf(const char *pf, unsigned side) {
    struct ethtool_drvinfo driver = {.cmd = ETHTOOL_GDRVINFO};
    int rc = ethctl(pf, &driver);
    if (rc < 0) return rc;
    if (strcmp(driver.driver, "ixgbe")) return -ENODEV;
    struct ethtool_value flags = {.cmd = ETHTOOL_GFLAGS};
    if ((rc = ethctl(pf, &flags)) < 0) return rc;
    if (!(flags.data & ETH_FLAG_NTUPLE)) return -EOPNOTSUPP;
    /* Empty tables avoid overwriting static demo filters or mixing PF masks. */
    struct ethtool_rxnfc table = {.cmd = ETHTOOL_GRXCLSRLALL};
    if ((rc = ethctl(pf, &table)) < 0) return rc;
    if (table.rule_cnt) return -EEXIST;
    capacity[side] = table.data & ~RX_CLS_LOC_SPECIAL;
    return capacity[side] ? 0 : -ENOSPC;
}

static int probe_vf(const char *pf, uint16_t vf, uint16_t port) {
    char path[PATH_MAX], resolved[PATH_MAX], device[RTE_ETH_NAME_MAX_LEN];
    if (vf >= 63 || port >= RTE_MAX_ETHPORTS || !ports->init[port]) return -EINVAL;
    snprintf(path, sizeof(path), "/sys/class/net/%s/device/virtfn%u", pf, vf);
    if (!realpath(path, resolved)) return -errno;
    const char *bdf = strrchr(resolved, '/');
    if (!bdf || rte_eth_dev_get_name_by_port(port, device) < 0 || strcmp(bdf + 1, device))
        return -ENODEV;
    return 0;
}

static int probe(void) {
    if (probed) return 0;
    const UpfScalingConfig *cfg = &g_upf_workers->config;
    if (control_fd < 0) control_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (control_fd < 0) return -errno;
    if (access(cfg->dn_route_helper, X_OK) < 0) return -errno;
    int rc = probe_pf(cfg->n3_pf, 0);
    if (rc == 0) rc = probe_pf(cfg->n6_pf, 1);
    if (rc < 0) return rc;
    if (capacity[0] < cfg->slot_count) return -ENOSPC;
    for (uint16_t i = 0; i < cfg->slot_count; i++) {
        const UpfWorkerSlotConfig *slot = &cfg->slots[i];
        if ((rc = probe_vf(cfg->n3_pf, slot->n3_vf, slot->n3_port)) < 0 ||
            (rc = probe_vf(cfg->n6_pf, slot->n6_vf, slot->n6_port)) < 0) return rc;
    }
    probed = 1;
    return 0;
}

static int filter(unsigned side, uint32_t location, uint16_t vf, struct in_addr address, int add) {
    const char *pf = side ? g_upf_workers->config.n6_pf : g_upf_workers->config.n3_pf;
    if (location >= capacity[side]) return -ENOSPC;
    struct ethtool_rxnfc command = {.cmd = add ? ETHTOOL_SRXCLSRLINS : ETHTOOL_SRXCLSRLDEL};
    command.fs.location = location;
    if (add) {
        /* Queue zero in VF vf; the VF selector is one-based in ring_cookie. */
        command.fs.ring_cookie = (uint64_t)(vf + 1) << ETHTOOL_RX_FLOW_SPEC_RING_VF_OFF;
        command.fs.flow_type = side ? IP_USER_FLOW : UDP_V4_FLOW;
        command.fs.h_u.tcp_ip4_spec.ip4dst = address.s_addr;
        command.fs.m_u.tcp_ip4_spec.ip4dst = UINT32_MAX;
        if (side) {
            command.fs.h_u.usr_ip4_spec.ip_ver = ETH_RX_NFC_IP4;
        } else {
            command.fs.h_u.tcp_ip4_spec.pdst = htons(2152);
            command.fs.m_u.tcp_ip4_spec.pdst = UINT16_MAX;
        }
        /* Do not replace a rule inserted by another administrator after probe. */
        struct ethtool_rxnfc existing = {.cmd = ETHTOOL_GRXCLSRULE};
        existing.fs.location = location;
        int rc = ethctl(pf, &existing);
        if (rc == 0) return -EEXIST;
        if (rc != -EINVAL && rc != -ENOENT) return rc;
    }
    return ethctl(pf, &command);
}

static int start_route_helper(const UpfSteerUpdate *update) {
    const UpfScalingConfig *cfg = &g_upf_workers->config;
    const UpfWorkerSlotConfig *slot = &cfg->slots[update->slot];
    char ue[INET_ADDRSTRLEN], via[INET_ADDRSTRLEN], mac[RTE_ETHER_ADDR_FMT_SIZE];
    inet_ntop(AF_INET, &update->ue_addr, ue, sizeof(ue));
    inet_ntop(AF_INET, &slot->n6_addr, via, sizeof(via));
    rte_ether_format_addr(mac, sizeof(mac), &ports->mac[slot->n6_port]);
    char *argv[] = {(char *)cfg->dn_route_helper, "--remote", (char *)cfg->dn_host,
        update->operation == UPF_STEER_SESSION_ADD ? "add" : "del",
        (char *)cfg->dn_interface, ue, via, mac, NULL};
    posix_spawnattr_t attr;
    int rc = posix_spawnattr_init(&attr);
    if (rc) return -rc;
    rc = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    if (!rc) rc = posix_spawnattr_setpgroup(&attr, 0);
    if (!rc) rc = posix_spawn(&helper_pid, cfg->dn_route_helper, NULL, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    if (rc) { helper_pid = 0; return -rc; }
    helper_deadline = rte_get_timer_cycles() + 15 * rte_get_timer_hz();
    helper_timed_out = 0;
    return 0;
}

static int apply(const UpfSteerUpdate *update) {
    if (update->operation == UPF_STEER_PROBE) return probe();
    if (!probed) return -EAGAIN;
    if (update->slot >= g_upf_workers->config.slot_count || !update->generation) return -EINVAL;
    const UpfWorkerSlotConfig *slot = &g_upf_workers->config.slots[update->slot];
    if (update->generation != g_upf_workers->runtime[update->slot].generation) return -ESTALE;
    int rc;
    if (update->operation == UPF_STEER_N3_ADD) {
        if (n3_generation[update->slot])
            return n3_generation[update->slot] == update->generation ? 0 : -ESTALE;
        rc = filter(0, update->slot, slot->n3_vf, slot->n3_addr, 1);
        if (!rc) n3_generation[update->slot] = update->generation;
        return rc;
    }
    uint32_t index = update->session_index;
    if (index >= UPF_MAX_SESSION_RULES || !update->ue_addr.s_addr ||
        update->ue_addr.s_addr == INADDR_BROADCAST || IN_MULTICAST(ntohl(update->ue_addr.s_addr))) return -EINVAL;
    if (sessions[index].generation && (sessions[index].generation != update->generation ||
        sessions[index].slot != update->slot || sessions[index].ue.s_addr != update->ue_addr.s_addr)) return -ESTALE;
    if (update->operation == UPF_STEER_SESSION_ADD) {
        if (!n3_generation[update->slot]) return -EAGAIN;
        if (!sessions[index].generation) {
            rc = filter(1, index, slot->n6_vf, update->ue_addr, 1);
            if (rc) return rc;
            sessions[index].generation = update->generation;
            sessions[index].slot = update->slot;
            sessions[index].ue = update->ue_addr;
        }
    } else if (update->operation != UPF_STEER_SESSION_DEL) return -EINVAL;
    else if (!sessions[index].generation) return 0; /* No owned filter or helper attempt. */
    /* An owned filter requires DN rollback, including after an uncertain add.
     * Remove the NIC entry only after route rollback is acknowledged. */
    return start_route_helper(update);
}

static void acknowledge(int result) {
    RTE_LOG(INFO, APP, "UPF steering op=%u slot=%u session=%u result=%d\n",
            pending.operation, pending.slot, pending.session_index, result);
    g_upf_workers->steer_result = result;
    __atomic_store_n(&g_upf_workers->steer_ack_seq, pending_sequence, __ATOMIC_RELEASE);
    pending_sequence = 0;
}

void onvm_upf_steer_poll(void) {
    if (!UpfWorkerRegistryIsConfigured()) return;
    if (helper_pid > 0) {
        int status;
        pid_t rc = waitpid(helper_pid, &status, WNOHANG);
        if (!rc) {
            if (!helper_timed_out && rte_get_timer_cycles() > helper_deadline) {
                kill(-helper_pid, SIGKILL);
                helper_timed_out = 1;
            }
            return;
        }
        if (rc < 0 && errno == EINTR) return;
        int result = helper_timed_out ? -ETIMEDOUT :
            rc < 0 ? -errno : WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -EIO;
        helper_pid = 0;
        /* SSH failure cannot prove when a remote add stopped. Reserve the UE
         * even if a later cleanup succeeds, so a late route cannot be reused. */
        if (result && pending.operation == UPF_STEER_SESSION_ADD)
            sessions[pending.session_index].uncertain_route = 1;
        if (!result && pending.operation == UPF_STEER_SESSION_DEL && sessions[pending.session_index].generation) {
            const UpfWorkerSlotConfig *slot = &g_upf_workers->config.slots[pending.slot];
            result = filter(1, pending.session_index, slot->n6_vf, pending.ue_addr, 0);
            if (!result && sessions[pending.session_index].uncertain_route) result = -EUCLEAN;
            if (!result) memset(&sessions[pending.session_index], 0, sizeof(sessions[0]));
        }
        acknowledge(result);
        return;
    }
    uint32_t sequence = __atomic_load_n(&g_upf_workers->steer_request_seq, __ATOMIC_ACQUIRE);
    if (sequence == __atomic_load_n(&g_upf_workers->steer_ack_seq, __ATOMIC_RELAXED)) return;
    pending_sequence = sequence;
    pending = g_upf_workers->steer_request;
    int result = apply(&pending);
    if (result || !helper_pid) acknowledge(result);
}

void onvm_upf_steer_shutdown(void) {
    if (helper_pid > 0) { kill(-helper_pid, SIGKILL); waitpid(helper_pid, NULL, 0); }
    if (control_fd >= 0) close(control_fd);
}
