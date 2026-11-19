#!/bin/sh
# Launch only after the complete versioned package has passed its checksum.
# Failure always leaves the factory startup chain and adbd untouched.
BASE=${LAUNCHER_BOOT_ROOT:-/mnt/UDISK/launcher/autoboot}
RELEASE=$BASE/current
MANUAL=0
[ "$1" = --manual ] && MANUAL=1
LOG=/tmp/launcherboot.log
HB=/tmp/launcherboot.hb
CHILD=
invalid_heartbeat_age=0
say() { echo "launcherboot: $*" >> "$LOG"; }

recover() {
    reason=$1
    trap - TERM INT HUP
    say "fallback: $reason"
    if [ -n "$CHILD" ]; then
        kill -TERM "$CHILD" 2>/dev/null
        sleep 2
        kill -KILL "$CHILD" 2>/dev/null
        wait "$CHILD" 2>/dev/null
    fi
    vendor_pids=$(pidof BranOS 2>/dev/null)
    if [ -z "$vendor_pids" ]; then
        # The factory watchdog checks every five seconds. Let it recover first.
        sleep 6
        vendor_pids=$(pidof BranOS 2>/dev/null)
    fi
    if [ -z "$vendor_pids" ]; then
        # Avoid racing its next restart against the intact backup's painter.
        watchdog_pids=$(pidof watchdog.sh 2>/dev/null)
        [ -z "$watchdog_pids" ] || kill -TERM $watchdog_pids 2>/dev/null
        sleep 1
        vendor_pids=$(pidof BranOS 2>/dev/null)
    fi
    if [ -n "$vendor_pids" ]; then
        kill -CONT $vendor_pids 2>/dev/null
        say "factory process resumed: $vendor_pids"
    elif [ -x "$RELEASE/BranOS" ]; then
        # This is the verified, intact backup. Qt touch needs /etc/profile.
        ( . /etc/profile; cd /usr/bin/data/etc/log || exit 1; "$RELEASE/BranOS" >> /tmp/launcherboot-vendor.log 2>&1 ) &
        say "factory backup started"
    fi
    # procd also sends TERM during an ordinary reboot. The documented ADB
    # factory switch removes enabled before stopping this service, so TERM
    # alone must not erase the next-boot choice.
    if [ "$MANUAL" -eq 0 ] && [ "$reason" != signal ]; then
        rm -f "$BASE/enabled"
        sync
        say "automatic takeover disabled for the next boot"
    elif [ "$reason" = signal ]; then
        say "stop signal: retained next-boot choice"
    fi
    rm -f "$HB"
    exit 1
}
trap 'recover signal' TERM INT HUP

if [ "$MANUAL" -eq 0 ] && [ ! -f "$BASE/enabled" ]; then
    exit 0
fi
if ! mount | grep -q ' on /mnt/UDISK type '; then
    say "UDISK not mounted; factory startup continues"
    exit 2
fi
if [ ! -x "$RELEASE/launcher" ] || [ ! -r "$RELEASE/atlas.bin" ] ||
   [ ! -x "$RELEASE/BranOS" ] || [ ! -r "$RELEASE/manifest.md5" ]; then
    say "package incomplete; factory startup continues"
    exit 2
fi
if ! ( cd "$RELEASE" && md5sum -cs manifest.md5 ); then
    say "package checksum failed; factory startup continues"
    exit 2
fi
rm -f "$HB"
say "package verified; starting launcher"
if [ -x /etc/wifi/wifi ] && ! pidof wpa_supplicant >/dev/null 2>&1; then
    /etc/wifi/wifi start
    say "factory Wi-Fi service requested"
fi
# BranOS powers/opens the PM UART during its own startup. If we freeze it too
# early, the sensor never starts streaming for the Launcher to read.
pm_wait=0
pm_ready=0
while [ "$pm_wait" -lt 45 ]; do
    for vendor_pid in $(pidof BranOS 2>/dev/null); do
        if ls -l "/proc/$vendor_pid/fd" 2>/dev/null | grep -q '/dev/ttyS3'; then
            pm_ready=1
            break
        fi
    done
    [ "$pm_ready" -eq 1 ] && break
    sleep 1
    pm_wait=$((pm_wait + 1))
done
if [ "$pm_ready" -eq 1 ]; then
    sleep 3
    say "factory PM UART initialized after $pm_wait s"
else
    say "factory PM UART did not initialize within 45 s"
fi
AUTOBOOT_GUARD=1 "$RELEASE/launcher" "$RELEASE/atlas.bin" &
CHILD=$!
elapsed=0
while :; do
    sleep 2
    if ! kill -0 "$CHILD" 2>/dev/null; then
        recover "launcher exited"
    fi
    read now ignored < /proc/uptime
    now=${now%%.*}
    if [ -r "$HB" ]; then
        read beat < "$HB"
        case "$beat" in
            ''|*[!0-9]*)
                invalid_heartbeat_age=$((invalid_heartbeat_age + 2))
                if [ "$invalid_heartbeat_age" -gt 15 ]; then
                    recover "invalid heartbeat persisted"
                fi
                continue ;;
        esac
        invalid_heartbeat_age=0
        age=$((now - beat))
        if [ "$age" -lt 0 ] || [ "$age" -gt 15 ]; then
            recover "launcher heartbeat stale: $age s"
        fi
    else
        elapsed=$((elapsed + 2))
        if [ "$elapsed" -gt 30 ]; then
            recover "launcher did not reach first frame"
        fi
    fi
done
