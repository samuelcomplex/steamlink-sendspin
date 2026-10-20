#!/bin/sh
# Steam Link Speaker, USB version.
#
# The Steam Link's own startup runs steamlink/factory_test/run.sh from a USB stick that's plugged
# in at boot (it's a hook for factory testing). This copies the speaker into RAM and starts it in
# the background, then returns so the Steam Link carries on starting up. Nothing is written to
# the Steam Link: without the stick, it starts exactly as before.

SPEAKER=$(cd "$(dirname "$0")/../speaker" && pwd)
DEV=$(grep ' /mnt/disk ' /proc/mounts | awk '{print $1}' | head -1)

rm -rf /tmp/sendspin
mkdir -p /tmp/sendspin
cp "$SPEAKER/sendspin-steamlink" "$SPEAKER/supervisor.sh" /tmp/sendspin/ || exit 0
chmod +x /tmp/sendspin/sendspin-steamlink /tmp/sendspin/supervisor.sh
echo "$DEV" > /tmp/sendspin/usb-device

if command -v setsid >/dev/null 2>&1; then
    setsid /tmp/sendspin/supervisor.sh usb </dev/null >/dev/null 2>&1 &
else
    nohup /tmp/sendspin/supervisor.sh usb </dev/null >/dev/null 2>&1 &
fi
exit 0
