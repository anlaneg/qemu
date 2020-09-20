#! /bin/bash
ROOT=`pwd`
br_name="br1"
tap_name="tap0"
ifconfig $br_name down 2>/dev/null
brctl delbr $br_name
brctl addbr $br_name
ifconfig $br_name up
ifconfig $br_name 10.10.10.1/24

brctl delif $br_name $tap_name
ip tuntap del $tap_name mode tap
ip tuntap add $tap_name mode tap
brctl addif $br_name $tap_name
ifconfig $tap_name up

#config vm :10.10.10.9/24
#$ROOT/x86_64-softmmu/qemu-system-x86_64 -m 1024 -boot d -enable-kvm -smp 3 -net nic -net user -hda $ROOT/../stretch-0.img --nographic
$ROOT/x86_64-softmmu/qemu-system-x86_64 -m 1024 -boot d -enable-kvm -smp 3 -net nic -net tap,ifname=$tap_name,script=no,downscript=no -hda $ROOT/../stretch-0.img #--nographic
