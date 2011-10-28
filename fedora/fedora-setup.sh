#!/bin/sh
# This script sets up things we want to ship with the OS tree.  It should
# NOT set up caches.  For example, do NOT run ldconfig in here.

set -e
set -x

echo gnomeos > /etc/hostname

cat > /etc/locale.conf <<EOF
LANG="en_US.UTF-8"
EOF

chpasswd <<EOF
root:root
EOF

# dbus
groupadd -r -g 81 dbus || :
useradd -u 81 -g 81 -s /sbin/nologin -r -d '/' dbus || :

# util-linux
rm -f /etc/mtab
ln -s /proc/mounts /etc/mtab

# initscripts
groupadd -g 22 -r -f utmp || :
# touch /var/log/wtmp /var/run/utmp /var/log/btmp
# chown root:utmp /var/log/wtmp /var/run/utmp /var/log/btmp
# chmod 664 /var/log/wtmp /var/run/utmp
# chmod 600 /var/log/btmp

# usbmuxd
groupadd -r usbmuxd -g 113 || :
useradd -r -g usbmuxd -d / -s /sbin/nologin -u 113 usbmuxd || :

# pa
groupadd -f -r pulse || :
useradd -r -s /sbin/nologin -d /var/run/pulse -g pulse pulse || :

# gdm
useradd -M -u 42 -d /var/lib/gdm -s /sbin/nologin -r gdm || :
usermod -d /var/lib/gdm -s /sbin/nologin gdm || :

# rtkit
groupadd -r -g 172 rtkit || :
useradd -r -l -u 172 -g rtkit -d /proc -s /sbin/nologin rtkit || :

# avahi
groupadd -r -g 70 avahi || :
useradd -r -l -u 70 -g avahi -d /var/run/avahi-daemon -s /sbin/nologin avahi || :

# NetworkManager-openconnect
groupadd -r nm-openconnect || :
useradd  -r -s /sbin/nologin -d / -M -g nm-openconnect nm-openconnect || :

# /etc/fstab ?
# /etc/machine-id ?

# create users and groups
# groups: dialout floppy cdrom tape
