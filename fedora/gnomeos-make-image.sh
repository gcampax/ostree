#!/bin/sh
# -*- indent-tabs-mode: nil; -*-
# Generate a root filesystem image
#
# Copyright (C) 2011 Colin Walters <walters@verbum.org>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

set -e
set -x

SRCDIR=`dirname $0`
WORKDIR=`pwd`

DEPENDS="yumdownloader rpm2cpio cpio qemu-img grubby"

for x in $DEPENDS; do
    if ! command -v $x; then
        cat <<EOF
Couldn't find required dependency $x
EOF
        exit 1
    fi
done

if test $(id -u) != 0; then
    cat <<EOF
This script must be run as root.
EOF
    exit 1
fi

if test -z "${OSTREE}"; then
    OSTREE=`command -v ostree || true`
fi
if test -z "${OSTREE}"; then
    cat <<EOF
ERROR:
Couldn't find ostree; you can set the OSTREE environment variable to point to it
e.g.: OSTREE=~user/checkout/ostree/ostree $0
EOF
    exit 1
fi

if test -z "$DRACUT"; then
    if ! test -d dracut; then
        cat <<EOF
Checking out and patching dracut...
EOF
        git clone git://git.kernel.org/pub/scm/boot/dracut/dracut.git
        (cd dracut; git am $SRCDIR/0001-Support-OSTree.patch)
        (cd dracut; make)
    fi
    DRACUT=`pwd`/dracut/dracut
fi

case `uname -p` in
    x86_64)
        ARCH=amd64
        ;;
    *)
        echo "Unsupported architecture"
        ;;
esac;

FEDTARGET=f16

INITRD_MOVE_MOUNTS="dev proc sys"
TOPROOT_BIND_MOUNTS="boot home root tmp"
OSTREE_BIND_MOUNTS="var"
MOVE_MOUNTS="selinux mnt media"
READONLY_BIND_MOUNTS="bin etc lib lib32 lib64 sbin usr"

OBJ=$FEDTARGET-pkgs
if ! test -d ${OBJ} ; then
    echo "Creating ${OBJ}"
    mkdir -p ${OBJ}.tmp
    for p in `cat ${SRCDIR}/package-list`; do
      yumdownloader --destdir ${OBJ}.tmp $p
    done
    mv ${OBJ}.tmp ${OBJ}
fi

OBJ=$FEDTARGET-fs
if ! test -d ${OBJ}; then
    if false; then
    rm -rf ${OBJ}.tmp
    mkdir ${OBJ}.tmp

    (cd ${OBJ}.tmp;
        for p in ../$FEDTARGET-pkgs/*.rpm; do
            rpm2cpio $p | cpio -dium --no-absolute-filenames
        done
    )
    fi

    (cd ${OBJ}.tmp;
     if false; then
        mkdir ostree
        mkdir ostree/repo
        mkdir ostree/gnomeos-origin
        for d in $INITRD_MOVE_MOUNTS $TOPROOT_BIND_MOUNTS; do
            mkdir -p ostree/gnomeos-origin/$d
            chmod --reference $d ostree/gnomeos-origin/$d
        done
        for d in $OSTREE_BIND_MOUNTS; do
            mkdir -p ostree/gnomeos-origin/$d
            chmod --reference $d ostree/gnomeos-origin/$d
            mv $d ostree
        done
        for d in $READONLY_BIND_MOUNTS $MOVE_MOUNTS; do
            if test -d $d; then
                mv $d ostree/gnomeos-origin
            fi
        done
     fi

        $OSTREE init --repo=ostree/repo
        (cd ostree/gnomeos-origin; find . '!' -type p | grep -v '^.$' | $OSTREE commit -s 'Initial import' --repo=../repo --from-stdin)
    )
    if test -d ${OBJ}; then
        mv ${OBJ} ${OBJ}.old
    fi
    mv ${OBJ}.tmp ${OBJ}
    rm -rf ${OBJ}.old
fi

# TODO download source for above
# TODO download build dependencies for above

OBJ=gnomeos-fs
if ! test -d ${OBJ}; then
    rm -rf ${OBJ}.tmp
    cp -al $FEDTARGET-fs ${OBJ}.tmp
    (cd ${OBJ}.tmp;

        cp ${SRCDIR}/fedora-setup.sh ostree/gnomeos-origin/
        chroot ostree/gnomeos-origin ./fedora-setup.sh
        rm ostree/gnomeos-origin/fedora-setup.sh
        (cd ostree/gnomeos-origin; find . '!' -type p | grep -v '^.$' | $OSTREE commit -s 'Run fedora-setup.sh' --repo=../repo --from-stdin)

        cp -p ${SRCDIR}/chroot_break ostree/gnomeos-origin/sbin/chroot_break
        (cd ostree/gnomeos-origin; $OSTREE commit -s 'Add chroot_break' --repo=../repo --add=sbin/chroot_break)

        (cd ostree;
            rev=`cat repo/HEAD`
            $OSTREE checkout --repo=repo HEAD gnomeos-${rev}
            $OSTREE run-triggers --repo=repo gnomeos-${rev}
            ln -s gnomeos-${rev} current)
    )
    if test -d ${OBJ}; then
        mv ${OBJ} ${OBJ}.old
    fi
    mv ${OBJ}.tmp ${OBJ}
    rm -rf ${OBJ}.old
fi

cp ${SRCDIR}/ostree_switch_root ${WORKDIR}

kv=`uname -r`
kernel=/boot/vmlinuz-${kv}
if ! test -f "${kernel}"; then
    cat <<EOF
Failed to find ${kernel}
EOF
fi

OBJ=gnomeos-initrd.img
VOBJ=gnomeos-initrd-${kv}.img
if ! test -f ${OBJ}; then
    rm -f ${OBJ}.tmp ${VOBJ}.tmp
    $DRACUT -l -v --include `pwd`/ostree_switch_root /sbin/ostree_switch_root ${VOBJ}.tmp
    mv ${VOBJ}.tmp ${VOBJ}
    ln -sf ${VOBJ} gnomeos-initrd.img
fi
