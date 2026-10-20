#!/bin/sh
# sendspin-steamlink device-side control. Run as root on the Steam Link:
#   ctl.sh install DIR   install/upgrade from unpacked DIR and start playing now
#   ctl.sh status        show what's installed and running
#   ctl.sh log           show the last lines of the player log
#   ctl.sh restart       restart the player
#   ctl.sh ui            boot back into the normal Steam Link interface (keeps the install)
#   ctl.sh speaker       boot as a Sendspin speaker again
#   ctl.sh uninstall     remove everything and restore the original startup

SS=/mnt/scratch/sendspin
CFG=/mnt/config/sendspin
STARTUP=/etc/init.d/startup/S99steam

overlay_dir() {
    # Where writes to /etc actually land (the Steam Link mounts /etc as a unionfs overlay).
    d=$(grep ' /etc ' /proc/mounts | sed -n 's/.*dirs=\([^=]*\)=rw.*/\1/p' | head -1)
    [ -z "$d" ] && [ -d /mnt/config/overlay/etc ] && d=/mnt/config/overlay/etc
    echo "$d"
}

stop_player() {
    # Every supervisor, old or new (not only the one in the pid file), then the player.
    if [ -f /var/run/sendspin-run.pid ]; then
        kill "$(cat /var/run/sendspin-run.pid)" 2>/dev/null
        rm -f /var/run/sendspin-run.pid
    fi
    for pid in $(ps | grep '[s]endspin/run\.sh' | awk '{print $1}'); do
        kill "$pid" 2>/dev/null
    done
    sleep 1
    killall sendspin-steamlink 2>/dev/null
    sleep 1
    killall -9 sendspin-steamlink 2>/dev/null
    for pid in $(ps | grep '[s]endspin/run\.sh' | awk '{print $1}'); do
        kill -9 "$pid" 2>/dev/null
    done
    true
}

# Copies a file into place by writing a temporary copy and renaming it over the old one, which
# works even while the old file is a running program ("text file busy" otherwise).
put() {
    cp -f "$1" "$2.new" && chmod +x "$2.new" && mv -f "$2.new" "$2"
}

start_player() {
    # setsid so the player survives the SSH session ending
    if command -v setsid >/dev/null 2>&1; then
        setsid $SS/run.sh </dev/null >/dev/null 2>&1 &
    else
        nohup $SS/run.sh </dev/null >/dev/null 2>&1 &
    fi
}

case "$1" in
install)
    SRC=$2
    OVL=$(overlay_dir)
    if [ -z "$OVL" ] || [ ! -d "$OVL" ]; then
        echo "ERROR: couldn't find the writable /etc overlay; nothing was changed."
        grep ' /etc ' /proc/mounts
        exit 1
    fi
    echo "/etc overlay: $OVL"
    mkdir -p $SS $CFG "$OVL/init.d/startup" || exit 1

    stop_player
    for f in sendspin-steamlink run.sh ctl.sh; do
        put "$SRC/$f" "$SS/$f" || { echo "ERROR: couldn't update $SS/$f"; exit 1; }
    done
    echo "Installed $(grep -m1 '^# version' $SS/run.sh)"
    # The black-screen option (-V, now default) is obsolete: drop it from kept settings.
    sed -i 's/^EXTRA_ARGS="-V"$/EXTRA_ARGS=""/' $SS/sendspin.conf 2>/dev/null
    if [ -f $SS/sendspin.conf ]; then
        echo "Keeping existing settings in $SS/sendspin.conf"
    else
        cp -f "$SRC/sendspin.conf" $SS/sendspin.conf
    fi

    # Back up the original startup script once, unless it's already ours. If the overlay
    # already held its own copy (not ours), keep that too so uninstall can put it back.
    if [ -f "$OVL/init.d/startup/S99steam" ] && [ ! -f $CFG/S99steam.overlay ] &&
       ! grep -q sendspin-steamlink "$OVL/init.d/startup/S99steam"; then
        cp -f "$OVL/init.d/startup/S99steam" $CFG/S99steam.overlay
    fi
    if [ ! -f $CFG/S99steam.orig ]; then
        if grep -q sendspin-steamlink $STARTUP 2>/dev/null; then
            echo "Startup script is already replaced; no backup needed."
        else
            cp -f $STARTUP $CFG/S99steam.orig || exit 1
            echo "Backed up original startup to $CFG/S99steam.orig"
        fi
    fi
    # Write through /etc, as the Steam Link's own USB "overlay" updates do; the union stores
    # the copy on the config partition.
    cp -f "$SRC/S99steam" $STARTUP.new && chmod +x $STARTUP.new && mv -f $STARTUP.new $STARTUP || exit 1
    touch $SS/enabled
    sync

    if ! grep -q sendspin-steamlink "$OVL/init.d/startup/S99steam" 2>/dev/null; then
        echo "ERROR: the new startup script didn't land on the config partition; undoing."
        rm -f $SS/enabled
        exit 1
    fi
    echo "Startup script replaced (stored in $OVL/init.d/startup/S99steam)."
    if pidof PE_Single_CPU >/dev/null 2>&1; then
        echo "Starting the player now (the Steam Link interface will close)..."
        start_player
        sleep 4
        tail -n 12 $SS/log.txt 2>/dev/null
    else
        echo "The Steam Link's media server isn't running (left over from the previous version)."
        echo "Rebooting it now; the player starts by itself about a minute after boot."
        sync
        reboot
    fi
    ;;
status)
    echo "--- install"
    ls -la $SS 2>/dev/null || echo "not installed"
    [ -f $SS/enabled ] && echo "mode: speaker (boots into Sendspin)" || echo "mode: Steam Link interface"
    grep -q sendspin-steamlink $STARTUP 2>/dev/null && echo "startup: replaced" || echo "startup: original"
    echo "--- processes"
    ps | grep -E '[s]endspin|[r]un.sh|[s]hell'
    echo "--- settings"
    cat $SS/sendspin.conf 2>/dev/null
    ;;
log)
    tail -n "${2:-40}" $SS/log.txt
    ;;
trace)
    tail -n "${2:-80}" $SS/trace.txt
    ;;
restart)
    stop_player
    start_player
    sleep 4
    tail -n 12 $SS/log.txt
    ;;
ui)
    rm -f $SS/enabled
    stop_player
    sync
    echo "The Steam Link will boot into its normal interface. Rebooting now..."
    reboot
    ;;
speaker)
    touch $SS/enabled
    sync
    echo "The Steam Link will boot as a Sendspin speaker. Rebooting now..."
    reboot
    ;;
uninstall)
    OVL=$(overlay_dir)
    rm -f $SS/enabled
    stop_player
    if [ -n "$OVL" ]; then
        if [ -f $CFG/S99steam.overlay ]; then
            cp -f $CFG/S99steam.overlay "$OVL/init.d/startup/S99steam"
        else
            rm -f "$OVL/init.d/startup/S99steam"
        fi
    fi
    rm -rf $SS $CFG
    sync
    echo "Removed. Rebooting into the original Steam Link interface..."
    reboot
    ;;
*)
    sed -n '2,10p' "$0"
    exit 2
    ;;
esac
