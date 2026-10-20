#!/bin/sh
# version 6 (2026-10-20): old-BusyBox-safe trap (v3-v5 died before starting the player)
#
# sendspin-steamlink supervisor: runs the player, restarts it if it exits, and keeps the
# Steam Link interface from taking the audio device back.
#
# Started at boot by /etc/init.d/startup/S99steam ("run.sh boot") after the original Steam Link
# startup (rc.local) has loaded the Marvell drivers and started the PE_Single_CPU media server,
# which the SDL "marvell" audio driver needs. Without "boot" it takes over immediately.

SS=/mnt/scratch/sendspin
LOG=$SS/log.txt
TRACE=$SS/trace.txt
PIDFILE=/var/run/sendspin-run.pid

sslog() { echo "=== $(date) $*" >> $LOG; }

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

NAME="Steam Link"
SERVER_URL=""
DELAY_MS=0
LATENCY_MS=0
EXTRA_ARGS=""
[ -f $SS/sendspin.conf ] && . $SS/sendspin.conf

sslog "supervisor version 6 starting ($*)"

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
stop_ui() {
    for pid in $(ps | grep -E '[s]hell\.sh|[p]owermanager' | awk '{print $1}'); do
        kill -STOP "$pid" 2>/dev/null
    done
    found=""
    for pid in $(ui_pids); do
        ppid=$(awk '{print $4}' /proc/$pid/stat 2>/dev/null)
        if [ -n "$ppid" ] && [ "$ppid" -gt 1 ]; then
            pcmd=$(tr '\0' ' ' < /proc/$ppid/cmdline 2>/dev/null)
            case "$pcmd" in
                *shell*|*steam*|*powermanager*|*rc.local*) kill -STOP "$ppid" 2>/dev/null ;;
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

if [ "$1" = boot ]; then
    # Wait for the media server, then for the interface (its start brings up HDMI).
    i=0
    while [ $i -lt 60 ] && ! pidof PE_Single_CPU >/dev/null 2>&1; do sleep 1; i=$((i + 1)); done
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
    while [ -f $SS/enabled ]; do
        if stop_ui "Steam Link interface was running again; stopped it, restarting the player"; then
            killall sendspin-steamlink 2>/dev/null
        fi
        sleep 3
    done
) &
WATCHER=$!
# Signal numbers, not names: the Steam Link's old BusyBox ash rejects "TERM" ("bad trap")
# and a failed trap ends the script.
trap 'kill $WATCHER 2>/dev/null; rm -f $PIDFILE; exit 0' 15 2

while [ -f $SS/enabled ]; do
    if [ -f $LOG ] && [ "$(wc -c < $LOG)" -gt 1000000 ]; then
        mv -f $LOG $LOG.1
    fi
    sslog "starting sendspin-steamlink"
    URL_ARG=""
    [ -n "$SERVER_URL" ] && URL_ARG="-u $SERVER_URL"
    # shellcheck disable=SC2086
    $SS/sendspin-steamlink -n "$NAME" $URL_ARG -s $SS/state -d "$DELAY_MS" -L "$LATENCY_MS" \
        $EXTRA_ARGS >> $LOG 2>&1
    sslog "player exited with code $?, restarting"
    sleep 2
done
kill $WATCHER 2>/dev/null
rm -f $PIDFILE
