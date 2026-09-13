# Scripts

This directory contains various scripts to run and configure different
portions of openNetVM

## Ports reserved for direct NF I/O

`start.sh -D DIRECT_PORTMASK` reserves ports for direct NF RX/TX. The manager
initializes one RX queue and one TX queue (both queue 0) on each reserved port,
but does not receive or transmit packets on it. Other ports keep the existing
manager/ring path. The default mask is `0`.

The direct mask must be a subset of `-k PORTMASK`. For four VF devices assigned
DPDK port IDs 0–3:

```bash
ONVM_ALLOW_LIST="<N3_VF1_PCI> <N6_VF1_PCI> <N3_VF2_PCI> <N6_VF2_PCI>" \
    ./scripts/start.sh -k f -D f -n 0xFFF8 -s stdout
```

Check the manager startup log for the actual device-to-port-ID mapping. Each
reserved port must have exactly one direct-I/O NF polling/transmitting queue 0;
a ring-based NF cannot use these ports through the manager.

`ONVM_DIRECT_PORT_MASK=f` also supplies the mask, allowing an existing wrapper
script to enable it without adding a `-D` argument. An explicit `-D` overrides
the environment variable. The manager publishes the immutable `uint64_t` mask
in `MProc_direct_port_mask` for direct-I/O NFs to check at startup.

## Licensing

```
# BSD LICENSE
#
# Copyright(c)
#          2015-2017 George Washington University
#          2015-2017 University of California Riverside
#          2010-2014 Intel Corporation.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#
# Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
# Redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in
# the documentation and/or other materials provided with the
# distribution.
# The name of the author may not be used to endorse or promote
# products derived from this software without specific prior
# written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
