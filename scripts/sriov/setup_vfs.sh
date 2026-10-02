#!/usr/bin/env bash
# run as: sudo bash ./setup_vfs.sh enp6s0f1 4 05
set -e

PF="$1"
NUM_VFS="$2"
MAC_ID="$3"

DPDK_DEVBIND="$HOME/dpdk-stable-25.11.2/usertools/dpdk-devbind.py"

if [[ -z "$PF" || -z "$NUM_VFS" || -z "$MAC_ID" ]]; then
    echo "Usage: sudo $0 <PF> <num_vfs> <mac_id>"
    echo
    echo "Examples:"
    echo "  sudo $0 enp6s0f1 4 05"
    echo "    -> 02:00:00:05:00:01 ..."
    echo
    echo "  sudo $0 enp6s0f1 4 04"
    echo "    -> 02:00:00:04:00:01 ..."
    exit 1
fi

if [[ ! -d "/sys/class/net/$PF" ]]; then
    echo "ERROR: PF $PF does not exist"
    exit 1
fi

if [[ ! -f "$DPDK_DEVBIND" ]]; then
    echo "ERROR: dpdk-devbind.py not found:"
    echo "$DPDK_DEVBIND"
    exit 1
fi

echo "Creating $NUM_VFS VFs on $PF..."

# Remove old VFs
echo 0 > "/sys/class/net/$PF/device/sriov_numvfs"
sleep 1

# Create VFs
echo "$NUM_VFS" > "/sys/class/net/$PF/device/sriov_numvfs"
sleep 1

# Configure VFs
for ((i=0; i<NUM_VFS; i++)); do
    VF_MAC=$(printf "02:00:00:%s:00:%02x" "$MAC_ID" $((i+1)))

    ip link set dev "$PF" vf "$i" mac "$VF_MAC"
    ip link set dev "$PF" vf "$i" spoofchk off trust on

    echo "VF $i -> $VF_MAC"
done

modprobe vfio-pci

echo
echo "Binding VFs to vfio-pci..."

for ((i=0; i<NUM_VFS; i++)); do
    VF_PATH="/sys/class/net/$PF/device/virtfn$i"

    if [[ ! -e "$VF_PATH" ]]; then
        echo "ERROR: virtfn$i not found"
        continue
    fi

    BDF=$(basename "$(readlink -f "$VF_PATH")")

    echo "VF $i -> $BDF"
    python3 "$DPDK_DEVBIND" -b vfio-pci "$BDF"
done

echo
echo "Done."
python3 "$DPDK_DEVBIND" -s
