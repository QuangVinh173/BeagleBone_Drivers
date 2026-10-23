sudo umount /dev/sda1 2>/dev/null || true

sudo sfdisk /dev/sda <<'EOF'
label: dos
unit: sectors

start=8192, type=c
EOF

sudo partprobe /dev/sda
sudo mkfs.vfat -F 32 -n BBB_USB /dev/sda1

# check
lsblk -f /dev/sda
sudo fdisk -l /dev/sda
