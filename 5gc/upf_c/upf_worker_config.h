/* SPDX-License-Identifier: Apache-2.0 */
#ifndef UPF_WORKER_CONFIG_H
#define UPF_WORKER_CONFIG_H

#include <stddef.h>
#include <yaml.h>
#include "upf_worker.h"

/* Parse configuration.scaling. Output is unchanged on failure.
 * Hardware/core availability is checked separately against manager state.
 */
int UpfScalingConfigParse(yaml_document_t *doc, yaml_node_t *mapping,
                          UpfScalingConfig *out, char *error, size_t error_size);

#endif
