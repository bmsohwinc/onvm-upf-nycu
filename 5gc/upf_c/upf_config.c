/*
# Copyright 2025 University of California, Riverside and National Yang Ming Chiao Tung University
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
*/

#include "upf_config.h"
#include "upf_worker_config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <rte_memzone.h>

#include "onvm_common.h"
#include "upf_events.h"
#include "upf_context.h"
#include "utlt_yaml.h"
#include "updk/env.h"

static int SetProtocolIter(YamlIter *protoList, YamlIter *protoIter);
// static Status ReadAddrList(YamlIter *protoIter, const char **hostname, int *num);
static void DeleteYamlDocument();

static Status AddGtpv1Endpoint(const char *host); // host: hostname or ip address
static Status AddPfcpEndpoint(const char *host); // host: hostname or ip address
static Status AddGtpv1EndpointWithName(const char *host, const char *ifname);

static yaml_document_t *document = NULL;

static Status ParseWorkerPortOrService(const char *value, unsigned long max, uint16_t *out) {
    char *end;
    UTLT_Assert(value && value[0] >= '0' && value[0] <= '9',
                return STATUS_ERROR, "Worker port/service must be a decimal integer");
    errno = 0;
    unsigned long n = strtoul(value, &end, 10);
    UTLT_Assert(!errno && !*end && n <= max, return STATUS_ERROR,
                "Invalid worker port/service: %s", value);
    *out = (uint16_t)n;
    return STATUS_OK;
}

static Status ParseWorkers(YamlIter *config) {
    YamlIter workers;
    YamlIterChild(config, &workers);
    UTLT_Assert(YamlIterType(&workers) == YAML_SEQUENCE_NODE && !Self()->workerCount,
                return STATUS_ERROR, "upf_u_workers must be one nonempty list");
    const struct rte_memzone *mz = rte_memzone_lookup(MZ_PORT_INFO);
    UTLT_Assert(mz && mz->len >= sizeof(struct port_info), return STATUS_ERROR,
                "Worker VFs require manager port information");
    const struct port_info *manager_ports = mz->addr;

    while (YamlIterNext(&workers)) {
        UTLT_Assert(Self()->workerCount < UPF_MAX_WORKERS, return STATUS_ERROR,
                    "Too many UPF-U workers");
        UpfWorker worker = {0};
        unsigned fields = 0;
        YamlIter entry;
        YamlIterChild(&workers, &entry);
        UTLT_Assert(YamlIterType(&entry) == YAML_MAPPING_NODE, return STATUS_ERROR,
                    "Each UPF-U worker must be a mapping");
        while (YamlIterNext(&entry)) {
            const char *key = YamlIterGet(&entry, GET_KEY);
            const char *value = YamlIterGet(&entry, GET_VALUE);
            unsigned field;
            UTLT_Assert(key && value, return STATUS_ERROR, "Invalid UPF-U worker field");
            if (!strcmp(key, "service_id")) {
                field = 1;
                UTLT_Assert(ParseWorkerPortOrService(value, MAX_SERVICES - 1,
                            &worker.service_id) == STATUS_OK, return STATUS_ERROR, "Invalid service_id");
            } else if (!strcmp(key, "n3_port") || !strcmp(key, "n6_port")) {
                field = !strcmp(key, "n3_port") ? 2 : 4;
                UTLT_Assert(ParseWorkerPortOrService(value, RTE_MAX_ETHPORTS - 1,
                            field == 2 ? &worker.n3_port : &worker.n6_port) == STATUS_OK,
                            return STATUS_ERROR, "Invalid VF port");
            } else if (!strcmp(key, "n3_ip")) {
                field = 8;
                UTLT_Assert(inet_pton(AF_INET, value, &worker.n3_addr) == 1 && worker.n3_addr.s_addr,
                            return STATUS_ERROR, "Invalid worker N3 IPv4 address: %s", value);
            } else if (!strcmp(key, "ul_teid")) {
                field = 16;
                char *end;
                UTLT_Assert(value[0] >= '0' && value[0] <= '9', return STATUS_ERROR,
                            "ul_teid must be a positive decimal or hexadecimal integer");
                errno = 0;
                int base = !strncmp(value, "0x", 2) || !strncmp(value, "0X", 2) ? 16 : 10;
                unsigned long teid = strtoul(value, &end, base);
                UTLT_Assert(!errno && !*end && teid && teid <= UINT32_MAX,
                            return STATUS_ERROR, "Invalid worker ul_teid: %s", value);
                worker.ul_teid = (uint32_t)teid;
            } else {
                UTLT_Error("Unknown UPF-U worker field: %s", key);
                return STATUS_ERROR;
            }
            UTLT_Assert(!(fields & field), return STATUS_ERROR, "Duplicate worker field: %s", key);
            fields |= field;
        }
        UTLT_Assert(fields == 31 && worker.service_id && worker.service_id != UPF_C_SERVICE_ID,
                    return STATUS_ERROR, "Worker requires service_id, n3_port, n6_port, n3_ip and ul_teid; service 2 is UPF-C");
        UTLT_Assert(worker.n3_port != worker.n6_port &&
                    manager_ports->init[worker.n3_port] && manager_ports->init[worker.n6_port],
                    return STATUS_ERROR, "Worker requires two distinct, manager-enabled VF ports");
        for (uint16_t i = 0; i < Self()->workerCount; i++) {
            const UpfWorker *other = &Self()->workers[i];
            UTLT_Assert(worker.service_id != other->service_id &&
                        worker.ul_teid != other->ul_teid &&
                        worker.n3_addr.s_addr != other->n3_addr.s_addr &&
                        worker.n3_port != other->n3_port && worker.n3_port != other->n6_port &&
                        worker.n6_port != other->n3_port && worker.n6_port != other->n6_port,
                        return STATUS_ERROR, "Workers must have distinct services, TEIDs, N3 IPs and VF pairs");
        }
        Self()->workers[Self()->workerCount++] = worker;
        UTLT_Info("UPF-U worker service=%u N3=%s port=%u N6 port=%u UL TEID=%u",
                    worker.service_id, inet_ntoa(worker.n3_addr), worker.n3_port, worker.n6_port, worker.ul_teid);
    }
    UTLT_Assert(Self()->workerCount, return STATUS_ERROR, "upf_u_workers must not be empty");
    return STATUS_OK;
}

static Status PublishWorkerSlots(void) {
    const UpfScalingConfig *config = &Self()->scaling;
    if (!config->slot_count) return STATUS_OK;
    UTLT_Assert(!Self()->workerCount, return STATUS_ERROR,
                "Use scaling.worker_slots or upf_u_workers, not both");
    UTLT_Assert(UpfWorkerRegistryAttach() == 0, return STATUS_ERROR,
                "Manager worker registry missing/incompatible; rebuild and restart manager");
    const struct rte_memzone *port_mz = rte_memzone_lookup(MZ_PORT_INFO);
    const struct rte_memzone *core_mz = rte_memzone_lookup(MZ_CORES_STATUS);
    const struct rte_memzone *service_mz = rte_memzone_lookup(MZ_NF_PER_SERVICE_INFO);
    UTLT_Assert(port_mz && port_mz->len >= sizeof(struct port_info) && core_mz && service_mz,
                return STATUS_ERROR, "Manager port/core/service information is missing");
    const struct port_info *manager_ports = port_mz->addr;
    const struct core_status *manager_cores = core_mz->addr;
    const uint16_t *service_counts = service_mz->addr;
    UTLT_Assert(config->rx_queue_threshold < NF_RX_QUEUE_RINGSIZE - 1 && config->rx_queue_threshold < NUM_MBUFS,
                return STATUS_ERROR, "RX queue threshold must be below usable RX capacity and dataplane mbuf count");
    for (uint16_t i = 0; i < config->slot_count; i++) {
        const UpfWorkerSlotConfig *slot = &config->slots[i];
        UTLT_Assert(slot->service_id < MAX_SERVICES && slot->service_id < g_upf_workers->service_limit &&
                    slot->service_id < service_mz->len / sizeof(*service_counts) &&
                    slot->service_id != UPF_C_SERVICE_ID && !service_counts[slot->service_id],
                    return STATUS_ERROR, "Slot %u service is reserved, occupied or outside manager limits", i);
        UTLT_Assert(slot->n3_port < RTE_MAX_ETHPORTS && slot->n6_port < RTE_MAX_ETHPORTS &&
                    manager_ports->init[slot->n3_port] && manager_ports->init[slot->n6_port],
                    return STATUS_ERROR, "Slot %u requires manager-initialized N3/N6 ports", i);
        UTLT_Assert(slot->core < core_mz->len / sizeof(*manager_cores) && manager_cores[slot->core].enabled &&
                    !manager_cores[slot->core].is_dedicated_core && !manager_cores[slot->core].nf_count,
                    return STATUS_ERROR, "Slot %u core is unavailable or not enabled for NFs", i);
    }
    UTLT_Assert(UpfWorkerRegistryPublish(config) == 0, return STATUS_ERROR,
                "Cannot publish worker slots; restart manager/UPF-C between configurations");
    UTLT_Info("Configured %u worker slots (min=%u max=%u RX threshold=%u sample_ms=%u consecutive=%u); all slots are INACTIVE",
               config->slot_count, config->min_workers, config->max_workers, config->rx_queue_threshold,
               config->queue_sample_interval_ms, config->queue_consecutive_samples);
    return STATUS_OK;
}

Status UpfLoadConfigFile(const char *configFilePath) {
    Status status = STATUS_OK;
    FILE *file;
    yaml_parser_t parser;

    file = fopen(configFilePath, "rb");
    UTLT_Assert(file, return STATUS_ERROR, "Fail to open yaml file");

    UTLT_Assert(yaml_parser_initialize(&parser), status = STATUS_ERROR; goto FREEFD, "Fail to initialize parser");
    yaml_parser_set_input_file(&parser, file);

    document = UTLT_Calloc(1, sizeof(yaml_document_t));
    
    UTLT_Assert(yaml_parser_load(&parser, document), UTLT_Free(document), "YAML parser load failed");

    yaml_parser_delete(&parser);

FREEFD:
    UTLT_Assert(!fclose(file), status = STATUS_ERROR, "Fail to close yaml file");

    return status;
}

Status UpfConfigParse() {
    UTLT_Assert(document, return STATUS_ERROR, "Config not loaded");

    YamlIter rootIter;

    YamlIterInit(&rootIter, document);
    while (YamlIterNext(&rootIter)) {
        const char *rootKey = YamlIterGet(&rootIter, GET_KEY);
        UTLT_Assert(rootKey, return STATUS_ERROR, "The rootKey is NULL");
        
        if (!strcmp(rootKey, "configuration")) {
            YamlIter upfIter;
            YamlIterChild(&rootIter, &upfIter);
            while (YamlIterNext(&upfIter)) {
                const char *upfKey = YamlIterGet(&upfIter, GET_KEY);
                UTLT_Assert(upfKey, return STATUS_ERROR, "The rootKey is NULL");

                if (!strcmp(upfKey, "debugLevel")) {
                    const char *logLevel = YamlIterGet(&upfIter, GET_VALUE);
                    
                    UTLT_Assert(UTLT_SetLogLevel(logLevel) == STATUS_OK,
                                return STATUS_ERROR, "");

                } else if (!strcmp(upfKey, "ReportCaller")) {
                    const char *reportCaller = YamlIterGet(&upfIter, GET_VALUE);
                    if (!strcmp(reportCaller, "true")) {
                        UTLT_Assert(UTLT_SetReportCaller(REPORTCALLER_TRUE) == STATUS_OK, return STATUS_ERROR, "");
                    } else if (!strcmp(reportCaller, "false")) {
                        UTLT_Assert(UTLT_SetReportCaller(REPORTCALLER_FALSE) == STATUS_OK, return STATUS_ERROR, "");
                    } else {
                        // Always fail here
                        UTLT_Assert(UTLT_SetReportCaller(REPORTCALLER_MAX) == STATUS_OK, return STATUS_ERROR, "ReportCaller is invalid");
                    }
                } else if (!strcmp(upfKey, "gtpu")) {
                    YamlIter gtpuList, gtpuIter;
                    YamlIterChild(&upfIter, &gtpuList);

                    do {
                        // int family = AF_INET;
                        // int i, hostCount = 0;
                        // const char *hostname[MAX_NUM_OF_HOSTNAME];
                        const char *host;
                        int port;
                        const char *ifname = NULL;

                        if (SetProtocolIter(&gtpuList, &gtpuIter)) {
                            break;
                        }

                        while (YamlIterNext(&gtpuIter)) {
                            const char *gtpuKey = YamlIterGet(&gtpuIter, GET_KEY);
                            UTLT_Assert(gtpuKey, return STATUS_ERROR, "The gtpuKey is NULL");

                            if (!strcmp(gtpuKey, "addr") || !strcmp(gtpuKey, "name")) {
                                /* UTLT_Assert(ReadAddrList(&gtpuIter, hostname, &hostCount) == STATUS_OK, 
                                            return STATUS_ERROR, "Failed to read gtpu address");*/
                                host = YamlIterGet(&gtpuIter, GET_VALUE);
                            } else if (!strcmp(gtpuKey, "family")) {
                                // TODO: support IPv6
                            } else if (!strcmp(gtpuKey, "port")) {
                                const char *v = YamlIterGet(&gtpuIter, GET_KEY);
                                if (v) {
                                    port = atoi(v);
                                    Self()->gtpv1Port = port;
                                }
                            } else if (!strcmp(gtpuKey, "ifname")) {
                                ifname = (char *)YamlIterGet(&gtpuIter, GET_VALUE);
                            } else {
                                UTLT_Warning("Unknown key \"%s\" of gtpu", gtpuKey);
                            }
                        }

                        if (host) {
                            if (ifname)
                                AddGtpv1EndpointWithName(host, ifname);
                            else
                                AddGtpv1Endpoint(host);
                        }
                        
                    } while (YamlIterType(&gtpuList) == YAML_SEQUENCE_NODE);
                    
                } else if (!strcmp(upfKey, "pfcp")) {
                    YamlIter pfcpList, pfcpIter;
                    YamlIterChild(&upfIter, &pfcpList);

                    do {
                        // int i, hostCount = 0;
                        // const char *hostname[MAX_NUM_OF_HOSTNAME];
                        const char *host;
                        
                        if (SetProtocolIter(&pfcpList, &pfcpIter))
                            break;
                        
                        while (YamlIterNext(&pfcpIter)) {
                            const char *pfcpKey = YamlIterGet(&pfcpIter, GET_KEY);
                            UTLT_Assert(pfcpKey, return STATUS_ERROR, "The pfcpKey is NULL");

                            if (!strcmp(pfcpKey, "addr") || !strcmp(pfcpKey, "name")) {
                                /* UTLT_Assert(ReadAddrList(&pfcpIter, hostname, &hostCount) == STATUS_OK, 
                                            return STATUS_ERROR, "Failed to read pfcp address"); */
                                host = YamlIterGet(&pfcpIter, GET_VALUE);
                            } else {
                                UTLT_Warning("Unknown key \"%s\" of pfcp", pfcpKey);
                            }
                        }

                        if (host)
                            AddPfcpEndpoint(host);

                    } while (YamlIterType(&pfcpList) == YAML_SEQUENCE_NODE);
                    
                } else if (!strcmp(upfKey, "upf_u_workers")) {
                    UTLT_Assert(ParseWorkers(&upfIter) == STATUS_OK, return STATUS_ERROR,
                                "Invalid upf_u_workers configuration");
                } else if (!strcmp(upfKey, "scaling")) {
                    YamlIter scaling;
                    char error[256];
                    UTLT_Assert(!Self()->scaling.slot_count, return STATUS_ERROR, "Duplicate scaling configuration");
                    YamlIterChild(&upfIter, &scaling);
                    UTLT_Assert(UpfScalingConfigParse(document, scaling.node, &Self()->scaling,
                                                     error, sizeof(error)) == 0,
                                return STATUS_ERROR, "Invalid scaling configuration: %s", error);
                } else if (!strcmp(upfKey, "dataplane_ports")) {
                    YamlIter portIter;
                    YamlIterChild(&upfIter, &portIter);
                    while (YamlIterNext(&portIter)) {
                        const char *portKey = YamlIterGet(&portIter, GET_KEY);
                        UTLT_Assert(portKey, return STATUS_ERROR, "The portKey is NULL");
                        const char *portVal = YamlIterGet(&portIter, GET_VALUE);

                        if (!strcmp(portKey, "access")) {
                            Self()->accessPort = atoi(portVal);
                        } else if (!strcmp(portKey, "core")) {
                            Self()->corePort = atoi(portVal);
                        } else if (!strcmp(portKey, "sgi")) {
                            Self()->sgiPort = atoi(portVal);
                        } else {
                            UTLT_Warning("Unknown key \"%s\" of dataplane_ports", portKey);
                        }
                    }
                    UTLT_Info("Dataplane ports: access=%d core=%d sgi=%d",
                             Self()->accessPort, Self()->corePort, Self()->sgiPort);

                } else if (!strcmp(upfKey, "dnn_list")) {
                    YamlIter dnnList, dnnIter;
                    YamlIterChild(&upfIter, &dnnList);

                    do {
                        const char *dnnName = NULL;
                        const char *ipStr = NULL;
                        const char *mask = NULL;
                        const char *natifname = NULL;

                        if (SetProtocolIter(&dnnList, &dnnIter))
                            break;

                        while (YamlIterNext(&dnnIter)) {
                            const char *dnnKey = YamlIterGet(&dnnIter, GET_KEY);
                            UTLT_Assert(dnnKey, return STATUS_ERROR, "The dnnKey is NULL");

                            if (!strcmp(dnnKey, "dnn")) {
                                dnnName = (char *)YamlIterGet(&dnnIter, GET_VALUE);
                            } else if (!strcmp(dnnKey, "cidr")) {
                                char *val = (char *)YamlIterGet(&dnnIter, GET_VALUE);
                                
                                if (val) {
                                    ipStr = (const char *)strsep(&val, "/");
                                    if (ipStr)
                                        mask = (const char *)val;
                                }
                            } else if (!strcmp(dnnKey, "natifname")) {
                                natifname = (char *)YamlIterGet(&dnnIter, GET_VALUE);
                            } else {
                                UTLT_Warning("Unknown key \"%s\" of dnn_list", dnnKey);
                            }
                        }

                        if (dnnName && ipStr && mask) {
                            DNN *dnn = AllocDNN();
                            UTLT_Assert(dnn, return STATUS_ERROR, "Alloc DNN failed")

                            UTLT_Assert(strlen(dnnName) < sizeof(dnn->name), return STATUS_ERROR,
                                "Length is too long for DNN name, Max is %u", sizeof(dnn->name) - 1);
                            strcpy(dnn->name, dnnName);

                            UTLT_Assert(strlen(ipStr) < sizeof(dnn->ipStr), return STATUS_ERROR,
                                "Length is too long for IP address, Max is %u", sizeof(dnn->ipStr) - 1);
                            strcpy(dnn->ipStr, ipStr);
                            
                            if (natifname) {
                                UTLT_Assert(strlen(natifname) < sizeof(dnn->natifname), return STATUS_ERROR,
                                    "Length is too long for NAT Ifname, Max is %u", sizeof(dnn->natifname) - 1);
                                strcpy(dnn->natifname, natifname);
                            }

                            dnn->subnetPrefix = atoi(mask);
                            EnvParamsAddDNN(Self()->envParams, dnn);
                        }
                    } while (YamlIterType(&dnnList) == YAML_SEQUENCE_NODE);
                } else
                    UTLT_Warning("Unknown key \"%s\" of configuration", upfKey);
            }
        }
    }

    DeleteYamlDocument();

    return PublishWorkerSlots();
}

static int SetProtocolIter(YamlIter *protoList, YamlIter *protoIter) {
    if (YamlIterType(protoList) == YAML_SCALAR_NODE) {
        return 1;
    } else if (YamlIterType(protoList) == YAML_SEQUENCE_NODE) {
        if (!YamlIterNext(protoList))
            return 1;
        YamlIterChild(protoList, protoIter);
    } else if (YamlIterType(protoList) == YAML_MAPPING_NODE) {
        memcpy(protoIter, protoList, sizeof(YamlIter));
    } else {
        UTLT_Assert(0, return 0, "Unknown node type");
        return 1;
    }

    return 0;
}

/* static Status ReadAddrList(YamlIter *protoIter, const char **hostname, int *num) {
    YamlIter hostnameIter;
    YamlIterChild(protoIter, &hostnameIter);
    UTLT_Assert(YamlIterType(&hostnameIter) != YAML_MAPPING_NODE, return STATUS_ERROR, "hostnameIter is type YAML_MAPPING_NODE");
    
    do {
        if (YamlIterType(&hostnameIter) == YAML_SEQUENCE_NODE) {
            if (!YamlIterNext(&hostnameIter))
                break;
        }
        UTLT_Assert(*num <= MAX_NUM_OF_HOSTNAME, return STATUS_ERROR, "hostnameIter is type YAML_MAPPING_NODE");
        
        hostname[(*num)++] = YamlIterGet(&hostnameIter, GET_VALUE);
    } while(YamlIterType(&hostnameIter) == YAML_SEQUENCE_NODE);

    return STATUS_OK;
} */

static void DeleteYamlDocument() {
    yaml_document_delete(document);
    UTLT_Free(document);
}

static Status AddGtpv1Endpoint(const char *host) {
    char ifname[MAX_IFNAME_STRLEN];
    sprintf(ifname, "%s", Self()->gtpDevNamePrefix);

    return AddGtpv1EndpointWithName(host, ifname);
}

static Status AddGtpv1EndpointWithName(const char *host, const char *ifname) {
    UTLT_Assert(host, return STATUS_ERROR, "");

    int result;
    char ipStr[INET6_ADDRSTRLEN];

    UTLT_Assert(strlen(ifname) < sizeof(Self()->envParams->virtualDevice->deviceID),
        return STATUS_ERROR, "ifname is too long");
    strcpy(Self()->envParams->virtualDevice->deviceID, ifname);

    result = GetAddrFromHost(ipStr, host, INET6_ADDRSTRLEN);
    UTLT_Assert(result == STATUS_OK, return STATUS_ERROR,
        "Cannot solve this hostname");

    // TODO: DO NOT handle DPDK now
    VirtualPort *port = AllocVirtualPort();
    UTLT_Assert(port, return STATUS_ERROR, "Alloc VirtualPort failed");

    strcpy(port->ipStr, ipStr);

    VirtualDeviceAddPort(Self()->envParams->virtualDevice, port);

    return STATUS_OK;
}

static Status AddPfcpEndpoint(const char *host) {
    UTLT_Assert(host, return STATUS_ERROR, "");

    int result;
    char ip[INET6_ADDRSTRLEN];

    result = GetAddrFromHost(ip, host, INET6_ADDRSTRLEN);
    UTLT_Assert(result == STATUS_OK, return STATUS_ERROR, "");

    SockNode *node = SockNodeListAdd(&Self()->pfcpIPList, ip);
    UTLT_Assert(node, return STATUS_ERROR, "");

    return STATUS_OK;
}
