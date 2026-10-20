#!/usr/bin/env bash
# steamlink-sendspin: turn an original Valve Steam Link into a Sendspin speaker for
# Music Assistant, playing over HDMI.
#
# Just run it and answer the questions:
#   bash steamlink-sendspin.sh
#
# Or give everything on the command line:
#   bash steamlink-sendspin.sh install   STEAMLINK_IP   install (or upgrade) and start playing
#   bash steamlink-sendspin.sh status    STEAMLINK_IP   what's installed and running
#   bash steamlink-sendspin.sh log       STEAMLINK_IP   the player's recent log
#   bash steamlink-sendspin.sh trace     STEAMLINK_IP   the supervisor's step-by-step trace
#   bash steamlink-sendspin.sh restart   STEAMLINK_IP   restart the player
#   bash steamlink-sendspin.sh ui        STEAMLINK_IP   boot back into the Steam Link interface
#   bash steamlink-sendspin.sh speaker   STEAMLINK_IP   boot as a speaker again
#   bash steamlink-sendspin.sh uninstall STEAMLINK_IP   remove everything, restore the original
#
# Settings for install (asked for when not given):
#   NAME="Living Room"                 player name in Music Assistant
#   SERVER_URL=192.168.1.6             Music Assistant's address (found automatically if possible)
#
# Each command asks for the Steam Link's SSH password once (steamlink123 unless changed).
# The player binary is embedded at the end of this file (build: @BUILD_INFO@).

set -eo pipefail

CMD="${1:-}"
HOST="${2:-}"
INTERACTIVE=0
[ -t 0 ] && INTERACTIVE=1

die() { echo "$*" >&2; exit 1; }
# ask PROMPT DEFAULT: prints the answer (the default when not on a terminal or left empty)
ask() {
    local reply=""
    if [ $INTERACTIVE = 1 ]; then
        if [ -n "$2" ]; then read -r -p "$1 [$2]: " reply; else read -r -p "$1: " reply; fi
    fi
    echo "${reply:-$2}"
}

command -v ssh >/dev/null || die "ssh isn't installed. Install it first (Debian/Ubuntu: sudo apt install openssh-client)."

# Steam Links (anything with SSH open) on this computer's local networks, named where possible.
find_steamlinks() {
    local nets ip
    nets=$(ip -4 -o addr show scope global 2>/dev/null | awk '{print $4}' | cut -d/ -f1 |
           grep -E '^(10|192\.168|172\.(1[6-9]|2[0-9]|3[01]))\.' | cut -d. -f1-3 | sort -u)
    for net in $nets; do
        for i in $(seq 1 254); do
            ( timeout 0.6 bash -c "exec 3<>/dev/tcp/$net.$i/22" 2>/dev/null && echo "$net.$i" ) &
        done
        wait
    done | sort -t. -k4 -n | while read -r ip; do
        name=$(getent hosts "$ip" | awk '{print $2}' | head -1)
        echo "$ip ${name:-}"
    done
}

if [ -z "$CMD" ]; then
    [ $INTERACTIVE = 1 ] || { sed -n '2,24p' "$0"; exit 2; }
    echo "What do you want to do?"
    echo "  1) Install or upgrade the speaker      5) Use as Steam Link again"
    echo "  2) Check status                         6) Use as speaker again"
    echo "  3) Show the player log                  7) Remove from Steam Link"
    echo "  4) Restart the player"
    case "$(ask "Choose" 1)" in
        1) CMD=install ;; 2) CMD=status ;; 3) CMD=log ;; 4) CMD=restart ;;
        5) CMD=ui ;; 6) CMD=speaker ;; 7) CMD=uninstall ;; *) die "Choose a number from 1 to 7." ;;
    esac
fi

if [ -z "$HOST" ]; then
    [ $INTERACTIVE = 1 ] || die "Give the Steam Link's address: bash $0 $CMD STEAMLINK_IP"
    echo "Searching this network for devices with SSH turned on..."
    mapfile -t FOUND < <(find_steamlinks)
    DEFAULT=""
    if [ ${#FOUND[@]} -gt 0 ]; then
        for i in "${!FOUND[@]}"; do
            echo "  $((i + 1))) ${FOUND[$i]}"
            case "${FOUND[$i],,}" in *steamlink*) [ -z "$DEFAULT" ] && DEFAULT=$((i + 1)) ;; esac
        done
        PICK=$(ask "Steam Link (number or address)" "${DEFAULT:-1}")
        if [[ "$PICK" =~ ^[0-9]+$ ]] && [ "$PICK" -ge 1 ] && [ "$PICK" -le ${#FOUND[@]} ]; then
            HOST=${FOUND[$((PICK - 1))]%% *}
        else
            HOST=$PICK
        fi
    else
        echo "Nothing found. If SSH isn't on yet: put an empty file steamlink/config/system/enable_ssh.txt"
        echo "on a FAT32 USB stick, plug it into the Steam Link and restart it."
        HOST=$(ask "Steam Link address" "")
    fi
    [ -n "$HOST" ] || die "No Steam Link address given."
fi

if [ "$CMD" = uninstall ] && [ $INTERACTIVE = 1 ]; then
    [[ "$(ask "Remove the speaker from $HOST and restore its original startup? (y/n)" n)" =~ ^[Yy] ]] ||
        die "Nothing changed."
fi

SSH=(ssh -o StrictHostKeyChecking=accept-new -o LogLevel=ERROR "root@$HOST")

# Music Assistant's Sendspin address: this computer if it runs Music Assistant, else what
# announces itself on the network (needs avahi-browse), else asked.
find_server() {
    local myip
    if ss -ltn 2>/dev/null | grep -qE '[:.]8927\b'; then
        myip=$(ip route get "$HOST" 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p' | head -1 || true)
        [ -n "$myip" ] && { echo "ws://$myip:8927/sendspin"; return; }
    fi
    if command -v avahi-browse >/dev/null; then
        avahi-browse -rpt _sendspin-server._tcp 2>/dev/null |
            awk -F';' '$1=="=" && $3=="IPv4" {print "ws://" $8 ":" $9 "/sendspin"; exit}'
    fi
}

case "$CMD" in
install)
    NAME="${NAME:-$(ask "Name in Music Assistant" "Steam Link")}"
    if [ -z "${SERVER_URL:-}" ]; then
        SERVER_URL=$(find_server || true)
        SERVER_URL=$(ask "Music Assistant address" "$SERVER_URL")
    fi
    case "$SERVER_URL" in
        ""|ws://*|wss://*) ;;
        *:*/*) SERVER_URL="ws://$SERVER_URL" ;;
        *:*) SERVER_URL="ws://$SERVER_URL/sendspin" ;;
        *) SERVER_URL="ws://$SERVER_URL:8927/sendspin" ;;
    esac
    NAME=$(printf '%s' "$NAME" | tr -d '"$`\\')
    SERVER_URL=$(printf '%s' "$SERVER_URL" | tr -d '"$`\\ ')
    echo "Player name:        $NAME"
    echo "Music Assistant at: ${SERVER_URL:-(not set; add the player in Music Assistant by IP)}"

    WORK=$(mktemp -d)
    trap 'rm -rf "$WORK"' EXIT
    sed -n '/^__PAYLOAD_BELOW__$/,$p' "$0" | tail -n +2 | base64 -d | tar xzf - -C "$WORK"
    cat > "$WORK/sendspin.conf" <<EOF
# sendspin-steamlink settings. After editing, run: steamlink-sendspin.sh restart <ip>

# Name shown in Music Assistant
NAME="$NAME"

# Music Assistant's Sendspin address; the Steam Link connects to it and reconnects if it restarts.
# Leave empty to only wait for Music Assistant to connect (add the player by IP there).
SERVER_URL="${SERVER_URL:-}"

# Sync tuning, in milliseconds. If this speaker plays LATE compared to your other
# speakers, raise LATENCY_MS (e.g. 50). DELAY_MS shifts playback the other way.
# Music Assistant's per-player delay setting also works and is remembered.
LATENCY_MS=0
DELAY_MS=0

# Extra options: -l debug for more logging.
EXTRA_ARGS=""

EOF
    tar czf - -C "$WORK" . | "${SSH[@]}" '
        rm -rf /tmp/ss-install && mkdir -p /tmp/ss-install &&
        tar xzf - -C /tmp/ss-install &&
        sh /tmp/ss-install/ctl.sh install /tmp/ss-install; rc=$?
        rm -rf /tmp/ss-install; exit $rc' ||
        echo "(connection closed: the Steam Link is rebooting)"
    echo
    echo "Installed. It should appear in Music Assistant as \"$NAME\"."
    echo "At boot the Steam Link shows its interface for about half a minute; then the player"
    echo "takes over. Check progress with: bash $0 log $HOST"
    ;;
status|log|trace|restart|ui|speaker|uninstall)
    "${SSH[@]}" "if [ -x /mnt/scratch/sendspin/ctl.sh ]; then /mnt/scratch/sendspin/ctl.sh $CMD; else echo 'sendspin-steamlink is not installed on this Steam Link'; fi"
    ;;
*)
    echo "Unknown command: $CMD"
    sed -n '5,16p' "$0"
    exit 2
    ;;
esac
exit 0

__PAYLOAD_BELOW__
