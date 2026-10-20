#!/bin/sh
# version 8 (2026-10-21): HDMI comes back after a TV/receiver off-on without dropping the stream
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
PAUSED=/var/run/sendspin-paused   # processes we paused, so they can be resumed
PELOG=/mnt/scratch/log/PE_server.log

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

sslog "supervisor version 8 starting ($*)"

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

# Sends a signal to the player: 12 (USR2) = let go of the audio device, 10 (USR1) = reopen it.
# The player stays connected to Music Assistant either way. Numbers, for old BusyBox.
player_signal() {
    for pid in $(pidof sendspin-steamlink 2>/dev/null); do kill -$1 "$pid" 2>/dev/null; done
}

# The display's last reported state in the media server's log: 1 = connected, 0 = gone.
hpd_state() {
    grep 'HPD=' $PELOG 2>/dev/null | tail -1 | sed -n 's/.*HPD=\([01]\).*/\1/p'
}

# Brings HDMI back the way it comes up at boot: lets the Steam Link interface start, gives it a
# few seconds, then stops it again. The player only lets go of the audio device meanwhile.
wake_hdmi() {
    sslog "display reconnected; letting the Steam Link interface set up HDMI again"
    player_signal 12
    for pid in $(cat $PAUSED 2>/dev/null); do kill -CONT "$pid" 2>/dev/null; done
    rm -f $PAUSED
    i=0
    while [ $i -lt 20 ] && [ -z "$(ui_pids)" ]; do sleep 1; i=$((i + 1)); done
    if [ -n "$(ui_pids)" ]; then
        sleep 8
        stop_ui "HDMI is set up; stopped the Steam Link interface again"
    else
        sslog "the interface didn't start within ${i}s; reopening audio anyway"
        stop_ui "stopped the Steam Link interface" || true
    fi
    sleep 1
    player_signal 10
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
    seen=$(hpd_count)
    while [ -f $SS/enabled ]; do
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
        elif stop_ui "Steam Link interface was running again; stopped it, reopening audio"; then
            sleep 1
            player_signal 10
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
