#! /bin/bash
apt-get install -y pkg-config zlib1g bison flex libseccomp-dev libglib2.0-dev libpixman-1-dev libaio-dev libnuma-dev libgnutls28-dev libnl-genl-3-dev vim make git
QEMU_CFLAGS="-g -O0" CFLAGS="-g -O0" CXXFLAGS="-g -O0" ./configure --enable-debug-info --target-list=x86_64-softmmu --enable-kvm --enable-vnc --enable-numa --disable-xen --enable-linux-aio --enable-seccomp --enable-gnutls --extra-ldflags="-Wl,-z,relro -Wl,-z,now -Wl,--disable-new-dtags -Bstatic -rdynamic" --extra-cflags="-Bstatic -rdynamic -g -O0" --extra-cxxflags="-g -O0"  --prefix="/" --disable-docs --disable-vnc
#make -j20 PKGVERSION=`git log | head -n 1 | tr " " "-"`
make -j PKGVERSION=`git log | head -n 1 | tr " " "-"`
