#!/bin/sh
# version 10 (2026-10-21): also runs from a USB stick ("usb" mode); finds Music Assistant itself
#
# sendspin-steamlink supervisor: runs the player, restarts it if it exits, and keeps the
# Steam Link interface from taking the audio device back.
#
# Installed version: started at boot by /etc/init.d/startup/S99steam ("run.sh boot") after the
# original Steam Link startup (rc.local) has loaded the Marvell drivers and started the
# PE_Single_CPU media server, which the SDL "marvell" audio driver needs. Without "boot" it takes
# over immediately.
#
# USB version: started early in boot by the stick's steamlink/factory_test/run.sh ("run.sh usb"),
# from a copy in RAM. It waits for the media server like the installed version, keeps its log and
# settings on the stick, and stops (handing the Steam Link back to its interface) as soon as the
# stick is pulled out. Nothing is written to the Steam Link itself.

MODE=installed
[ "$1" = usb ] && MODE=usb
PIDFILE=/var/run/sendspin-run.pid
PAUSED=/var/run/sendspin-paused   # processes we paused, so they can be resumed
PELOG=/mnt/scratch/log/PE_server.log

if [ $MODE = usb ]; then
    SS=/tmp/sendspin                       # the player and this script, in RAM
    USBDEV=$(cat $SS/usb-device 2>/dev/null)
    USBSYS=${SENDSPIN_USB_SYS:-/sys/class/block/${USBDEV##*/}}
    USBMNT=/mnt/sendspin-usb
    DATA=$SS                               # log and state; moved to the stick once it's mounted
else
    SS=/mnt/scratch/sendspin
    DATA=$SS
fi
LOG=$DATA/log.txt
TRACE=$SS/trace.txt

sslog() { echo "=== $(date) $*" >> $LOG; }

# Still meant to be running: installed and enabled, or the USB stick still plugged in.
active() {
    if [ $MODE = usb ]; then [ -n "$USBDEV" ] && [ -e "$USBSYS" ]; else [ -f $SS/enabled ]; fi
}

# Step-by-step trace of this script (kept small), for diagnosing startup problems.
[ -f $TRACE ] && [ "$(wc -c < $TRACE)" -gt 200000 ] && mv -f $TRACE $TRACE.1
exec 2>>$TRACE
echo "##### $(date) run.sh $* (pid $$)" >&2
set -x

# Only one supervisor at a time.
if [ -f $PIDFILE ] && kill -0 "$(cat $PIDFILE)" 2>/dev/null; then
    echo "another supervisor is running" >&2
    exit 0
fi
echo $$ > $PIDFILE

if [ $MODE = usb ]; then
    sslog "supervisor version 10 starting (USB stick $USBDEV)"
    if [ -f /mnt/scratch/sendspin/enabled ]; then
        sslog "this Steam Link has the installed version; the USB stick isn't needed. Stopping."
        rm -f $PIDFILE
        exit 0
    fi
    # The Steam Link's own startup reads the stick at /mnt/disk and unmounts it when it's done;
    # then mount it ourselves for the log and settings.
    i=0
    while [ $i -lt 60 ] && grep -q ' /mnt/disk ' /proc/mounts; do sleep 1; i=$((i + 1)); done
    mkdir -p $USBMNT
    if mount "$USBDEV" $USBMNT 2>>$TRACE && [ -d $USBMNT/steamlink/speaker ]; then
        cat $LOG >> $USBMNT/steamlink/speaker/log.txt 2>/dev/null
        DATA=$USBMNT/steamlink/speaker
        LOG=$DATA/log.txt
        [ -f $LOG ] && [ "$(wc -c < $LOG)" -gt 500000 ] && mv -f $LOG $DATA/log-old.txt
        [ -f $DATA/settings.txt ] && tr -d '\r' < $DATA/settings.txt > $SS/sendspin.conf
        sslog "log and settings on the USB stick"
    else
        sslog "couldn't mount the USB stick for the log; keeping it in RAM"
    fi
else
    sslog "supervisor version 10 starting ($*)"
fi

NAME=""
SERVER_URL=""
DELAY_MS=0
LATENCY_MS=0
EXTRA_ARGS=""
[ -f $SS/sendspin.conf ] && . $SS/sendspin.conf

# Live (not zombie) Steam Link interface processes. A killed interface whose parent is paused
# stays behind as a zombie, and BusyBox pidof still lists it.
ui_pids() {
    for pid in $(pidof shell 2>/dev/null); do
        state=$(awk '{print $3}' /proc/$pid/stat 2>/dev/null)
        [ -n "$state" ] && [ "$state" != Z ] && echo "$pid"
    done
}

# Pauses the scripts that (re)launch the Steam Link interface or put the device to sleep, then
# stops the interface ("shell") itself. Logs $1 and returns 0 if the interface was running.
pause() {
    kill -STOP "$1" 2>/dev/null || return
    grep -q "^$1\$" $PAUSED 2>/dev/null || echo "$1" >> $PAUSED
}

stop_ui() {
    for pid in $(ps | grep -E '[s]hell\.sh|[p]owermanager' | awk '{print $1}'); do
        pause "$pid"
    done
    found=""
    for pid in $(ui_pids); do
        ppid=$(awk '{print $4}' /proc/$pid/stat 2>/dev/null)
        if [ -n "$ppid" ] && [ "$ppid" -gt 1 ]; then
            pcmd=$(tr '\0' ' ' < /proc/$ppid/cmdline 2>/dev/null)
            case "$pcmd" in
                *shell*|*steam*|*powermanager*|*rc.local*) pause "$ppid" ;;
            esac
        fi
        kill "$pid" 2>/dev/null
        found=1
    done
    if [ -n "$found" ]; then
        sleep 1
        for pid in $(ui_pids); do kill -9 "$pid" 2>/dev/null; done
        sslog "$1"
        return 0
    fi
    return 1
}

# How many times the media server has seen a display connect ("HPD detected"). When a TV or
# receiver is switched off and on, HDMI drops and comes back, but nothing switches the output on
# again: that's done by the Steam Link interface, which we keep stopped.
hpd_count() {
    n=$(grep -c 'HPD detected' $PELOG 2>/dev/null)
    echo "${n:-0}"
}

# The display's last reported state in the media server's log: 1 = connected, 0 = gone.
hpd_state() {
    grep 'HPD=' $PELOG 2>/dev/null | tail -1 | sed -n 's/.*HPD=\([01]\).*/\1/p'
}

# USB version, stick removed: resume the Steam Link interface and its power manager, and let go
# of the stick. Safe to run more than once.
hand_back() {
    [ -f $PAUSED ] || return 0
    echo "=== $(date) USB stick removed; handing the Steam Link back to its interface" \
        >> $SS/log.txt
    for pid in $(cat $PAUSED 2>/dev/null); do kill -CONT "$pid" 2>/dev/null; done
    rm -f $PAUSED
    umount -l $USBMNT 2>/dev/null
}

# Brings HDMI back the way it comes up at boot: lets the Steam Link interface start, gives it a
# few seconds, then stops it again and restarts the player. The player says goodbye as
# "restart", so Music Assistant keeps it in its group and puts it back into the running stream.
wake_hdmi() {
    sslog "display reconnected; letting the Steam Link interface set up HDMI again"
    for pid in $(cat $PAUSED 2>/dev/null); do kill -CONT "$pid" 2>/dev/null; done
    rm -f $PAUSED
    i=0
    while [ $i -lt 20 ] && [ -z "$(ui_pids)" ]; do sleep 1; i=$((i + 1)); done
    if [ -n "$(ui_pids)" ]; then
        sleep 8
        stop_ui "HDMI is set up; stopped the Steam Link interface again"
    else
        sslog "the interface didn't start within ${i}s"
        stop_ui "stopped the Steam Link interface" || true
    fi
    sslog "restarting the player"
    killall sendspin-steamlink 2>/dev/null
}

if [ "$1" = boot ] || [ $MODE = usb ]; then
    # Wait for the media server, then for the interface (its start brings up HDMI). From USB we
    # start much earlier in boot, before the Steam Link's own startup has run at all.
    i=0
    while [ $i -lt 180 ] && ! pidof PE_Single_CPU >/dev/null 2>&1; do sleep 1; i=$((i + 1)); done
    sslog "media server $(pidof PE_Single_CPU >/dev/null 2>&1 && echo up || echo 'NOT running') after ${i}s"
    i=0
    while [ $i -lt 90 ] && [ -z "$(ui_pids)" ]; do sleep 1; i=$((i + 1)); done
    if [ -n "$(ui_pids)" ]; then
        sslog "Steam Link interface up after ${i}s; letting it finish starting"
        sleep 8
    else
        sslog "Steam Link interface didn't appear within ${i}s; starting anyway"
    fi
fi

stop_ui "stopped the Steam Link interface"

# Keep watching: if the interface comes back (e.g. after a wake-up), stop it again and
# restart the player so it reopens the HDMI audio device.
(
    set +x
    seen=$(hpd_count)
    while active; do
        now=$(hpd_count)
        if [ "$now" -gt "$seen" ] 2>/dev/null; then
            # Receivers often drop and restore the link several times while they power up and
            # pick an input: wait until it has been quiet for a few seconds.
            quiet=0
            while [ $quiet -lt 6 ]; do
                sleep 2
                n=$(hpd_count)
                if [ "$n" != "$now" ]; then now=$n; quiet=0; else quiet=$((quiet + 2)); fi
            done
            if [ "$(hpd_state)" = 1 ]; then
                wake_hdmi
                sleep 5
            else
                sslog "display disconnected; waiting for it to come back"
            fi
            seen=$(hpd_count)   # starting the interface can itself log a reconnect
        elif [ "$now" -lt "$seen" ] 2>/dev/null; then
            seen=$now           # the media server's log was restarted
        elif stop_ui "Steam Link interface was running again; stopped it, restarting the player"; then
            killall sendspin-steamlink 2>/dev/null
        fi
        sleep 3
    done
    if [ $MODE = usb ]; then
        # The stick was pulled out: give the Steam Link back to its interface first, then stop
        # the player (which ends the main loop).
        hand_back
        killall sendspin-steamlink 2>/dev/null
    fi
) &
WATCHER=$!
# Signal numbers, not names: the Steam Link's old BusyBox ash rejects "TERM" ("bad trap")
# and a failed trap ends the script.
trap 'kill $WATCHER 2>/dev/null; rm -f $PIDFILE; exit 0' 15 2

while active; do
    if [ -f $LOG ] && [ "$(wc -c < $LOG)" -gt 1000000 ]; then
        mv -f $LOG $LOG.1
    fi
    sslog "starting sendspin-steamlink"
    # Without a name the player calls itself "Steam Link XXXX" (end of its MAC address); without
    # an address it finds Music Assistant on the network itself.
    set --
    [ -n "$NAME" ] && set -- -n "$NAME"
    [ -n "$SERVER_URL" ] && set -- "$@" -u "$SERVER_URL"
    # shellcheck disable=SC2086
    $SS/sendspin-steamlink "$@" -s $DATA/state -d "$DELAY_MS" -L "$LATENCY_MS" $EXTRA_ARGS \
        >> $LOG 2>&1
    rc=$?
    active || break
    sslog "player exited with code $rc, restarting"
    sleep 2
done
[ $MODE = usb ] && hand_back
kill $WATCHER 2>/dev/null
rm -f $PIDFILE
