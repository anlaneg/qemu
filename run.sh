#! /bin/bash
ROOT=`pwd`
$ROOT/x86_64-softmmu/qemu-system-x86_64 -m 1024 -boot d -enable-kvm -smp 3 -net nic -net user -hda $ROOT/../stretch-0.img --nographic
