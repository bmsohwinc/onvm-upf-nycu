#!/usr/bin/env bash
# run as: sudo bash ./remove_vfs.sh enp6s0f1
set -e

PF="$1"

if [[ -z "$PF" ]]; then
    echo "Usage: sudo $0 <PF>"
    echo "Example: sudo $0 enp6s0f1"
    exit 1
fi

if [[ ! -d "/sys/class/net/$PF" ]]; then
    echo "ERROR: PF $PF does not exist"
    exit 1
fi

echo "Removing NIC flow rules from $PF..."

# Delete all ethtool RX/Flow-Director rules
RULES=$(ethtool -u "$PF" 2>/dev/null | awk '/Filter:/ {print $2}' || true)

for rule in $RULES; do
    echo "Deleting rule $rule"
    ethtool -U "$PF" delete "$rule" || true
done

echo
echo "Unbinding existing VFs..."

for VF_PATH in /sys/class/net/"$PF"/device/virtfn*; do
    [[ -e "$VF_PATH" ]] || continue

    BDF=$(basename "$(readlink -f "$VF_PATH")")

    if [[ -e "/sys/bus/pci/drivers/vfio-pci/$BDF" ]]; then
        echo "Unbinding $BDF from vfio-pci"
        echo "$BDF" > /sys/bus/pci/drivers/vfio-pci/unbind
    fi
done

echo
echo "Deleting all VFs..."
echo 0 > "/sys/class/net/$PF/device/sriov_numvfs"

echo
echo "Done."
ip link show "$PF"
