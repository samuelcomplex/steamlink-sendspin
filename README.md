# Steam Link Speaker

Turn an original Valve Steam Link into a [Music Assistant](https://music-assistant.io) speaker.
It plays synchronized audio over HDMI to whatever the Steam Link is plugged into: a TV, a
soundbar or an AV receiver.

The Steam Link keeps its stock firmware. Nothing is flashed. There are two ways to use it:

- **[USB stick](#usb-stick-no-installation):** the speaker runs from a USB stick for as long as
  the stick is plugged in. No installation, no settings, no computer setup.
- **[Installed](#install):** an installer puts a small [Sendspin](https://www.sendspin-audio.com/)
  player and one startup file on the Steam Link, so it works without a stick. It can remove both
  again.

Both need an original Valve Steam Link (the small black box, model 1001) on the stock firmware,
and Music Assistant **2.10.x** with its Sendspin provider and "Allow legacy clients" on (the
default); see [Music Assistant versions](#music-assistant-versions).

## USB stick (no installation)

1. Download `steamlink-speaker-usb.zip` from the [latest release](../../releases/latest).
2. Unzip it onto a FAT32 USB stick, so the stick has a `steamlink` folder at the top.
3. Plug the stick into the Steam Link and restart it (unplug its power for a moment).

The Steam Link starts as usual and shows its menu for about a minute; then the speaker takes over
and appears in Music Assistant as "Steam Link" plus four characters from its network address. It
finds Music Assistant on its own.

**To go back to normal, pull out the stick.** The Steam Link menu returns within a few seconds,
and nothing on the Steam Link has changed.

Optional: `steamlink\speaker\settings.txt` on the stick sets the name, Music Assistant's address
and sync tuning; edit it with any text editor and restart the Steam Link. The speaker writes its
log to `steamlink\speaker\log.txt` on the stick.

How it works: at startup the Steam Link runs `steamlink/factory_test/run.sh` from a plugged-in
stick, a hook meant for factory testing. That script copies the speaker into RAM and starts it,
and the speaker stops and hands the Steam Link back to its menu when the stick disappears.

## What you need (installed version)

- SSH turned on for the Steam Link:
  1. On a FAT32 USB stick, create the folders `steamlink\config\system`.
  2. In that folder, create an empty file named `enable_ssh.txt`.
  3. Plug the stick into the Steam Link and unplug its power for a moment to restart it.

  The SSH password is `steamlink123` unless you've changed it.
- A Windows or Linux computer on the same network.

## Install

Download from the [latest release](../../releases/latest).

### Windows

Run `SteamLinkSpeaker.exe`. A page opens in your browser:

1. **Find your Steam Link.** It searches your network and lists Steam Links first, recognized
   by their Valve network hardware.
2. **Sign in** with the Steam Link's SSH password.
3. **Choose how it plays**: the name to show in Music Assistant, and Music Assistant's address.
   It's filled in automatically when Music Assistant is found on the network; if it's left
   empty, the speaker looks for Music Assistant itself.

Then select **Install on Steam Link**. The player appears in Music Assistant within a few seconds.

The app isn't code-signed, so Windows shows "Windows protected your PC" the first time. Select
**More info**, then **Run anyway**. The app only serves its page to your own computer and quits
when you close the tab.

### Linux

Run the script and answer its questions:

```sh
bash steamlink-sendspin.sh
```

Or give everything at once, for example from the Music Assistant server:

```sh
NAME="Living Room" bash steamlink-sendspin.sh install 192.168.1.50
```

Prefer the browser app? `steamlink-speaker-linux-x64` (or `-arm64`, for a Raspberry Pi) is the
same app as the Windows one.

## After installing

At boot the Steam Link starts as usual, shows its interface for about half a minute, and then
the speaker takes over. The TV keeps showing the last frame; that's expected.

When the TV or receiver is switched off and on again, HDMI has to be set up again. The speaker
notices, lets the Steam Link interface start for about 10 seconds to do that, and then takes
over again, so there's no need to restart the Steam Link. That speaker is silent for about
15 seconds and then rejoins the music in sync by itself; other speakers in the same group keep
playing throughout. The same goes for restarting the Steam Link while music is playing.

From the app's **Manage** section, or with the script:

| Action | App button | Script command |
| --- | --- | --- |
| See what's installed and running | Check status | `status` |
| Read the player's log | Show player log | `log` |
| Restart the player | Restart player | `restart` |
| Boot the normal Steam Link interface, keep the install | Use as Steam Link again | `ui` |
| Boot as a speaker again | Use as speaker again | `speaker` |
| Remove everything and restore the original startup | Remove from Steam Link | `uninstall` |

Install again at any time to upgrade; your settings are kept.

## Receiver volume over HDMI

If the Steam Link is plugged into an AV receiver, Music Assistant's volume can turn the
receiver's own volume up and down over HDMI (CEC), instead of making the audio quieter. Tick
**Control the receiver's volume over HDMI** in the app before installing, or set
`AVR_VOLUME_STEPS=50` in the settings (`settings.txt` on the USB stick) and restart.

- The receiver needs its HDMI control (CEC) setting turned on.
- CEC can only step the volume, not set it, so the speaker follows how far the slider moves:
  `AVR_VOLUME_STEPS` presses for the whole 0-100 range. The volume Music Assistant shows when
  the speaker starts is taken as where the receiver already is; if they drift apart (someone
  used the receiver's remote), move the slider to the end and back.
- Mute is done by the speaker itself, so it never gets out of step with the receiver.
- Leave it off for a TV or soundbar: if they ignore the commands, the volume slider would do
  nothing.

`slcec`, installed next to the player, sends the commands and can be used by hand over SSH:
`slcec volup 3`, `slcec voldown`, `slcec mute`, `slcec standby`.

## Troubleshooting

**No sound.** Check the player log. A healthy boot looks like this:

```
supervisor version 10 starting (boot)
media server up after 0s
Steam Link interface up after 2s; letting it finish starting
stopped the Steam Link interface
starting sendspin-steamlink
...
>>> server connected
```

The `trace` command (script) shows the supervisor's step-by-step trace if it stops earlier.

**Music Assistant moved to another address.** Nothing to do: when the old address stops
answering, the speaker finds Music Assistant on the network within about two minutes. (If your
network blocks that kind of discovery, install again to set the new address.)

**This speaker is out of sync with the others.** Set the player's delay in Music Assistant, or
`LATENCY_MS` in `/mnt/scratch/sendspin/sendspin.conf` on the Steam Link, then restart the player.
The TV or receiver adds its own delay (more with picture processing on; a "game" mode or
audio-only input helps), which the Steam Link can't measure.

**Clicks or short dropouts.** Each one shows in the player log as `Lost sync (... off)`. The log
also gets an `audio timing` line every minute for the first ten minutes; please include a few of
them if you report this.

**Back to normal, whatever state it's in.** Use **Remove from Steam Link** (or `uninstall`). As
a last resort, the Steam Link's own factory reset works too: an empty `factory_reset.txt` in a
`steamlink` folder on a USB stick, then restart it.

**Security.** SSH stays on with the password it had. To change it, sign in with
`ssh root@<steamlink-address>` and run `passwd`; then use the new password in the installer.

## Music Assistant versions

Music Assistant 2.10.x speaks a Sendspin protocol version from before encryption was finalized.
This release is built for it and connects as a "legacy" (unencrypted) player, which 2.10.x
accepts while "Allow legacy clients" is on.

Later Music Assistant versions move to the encrypted Sendspin 1.0 protocol and will eventually
drop legacy players. `./build.sh encrypted` builds the player for those; it completes the
encrypted handshake with the newer server library but hasn't been tested end to end yet.

## How it works

- **The player** (`src/`) is built on [sendspin-cpp](https://github.com/Sendspin/sendspin-cpp)
  v0.8.0. It plays through the firmware's own SDL2 library and its `marvell` audio driver, loaded
  at runtime, so it needs nothing beyond what the Steam Link already has. It's cross-compiled
  with zig for the Steam Link's ARMv7 CPU and glibc 2.19 (Valve's SDK compiler is too old for
  C++20), linked with `-z lazy` because the firmware's loader crashes on bind-now executables.
- **Startup** (`device/S99steam`): the firmware mounts `/etc` as an overlay on its config
  partition, so the installer replaces one startup script there and backs up the original.
  The original startup always runs first, because it loads the Marvell drivers and starts the
  `PE_Single_CPU` media server that HDMI audio depends on.
- **The supervisor** (`device/run.sh`) waits for the media server and the Steam Link
  interface, pauses the power manager and the interface's relaunch script, closes the
  interface, and runs the player. It restarts the player if it exits, and takes over again if
  the interface comes back. When the media server logs a display reconnect (`HPD detected` in
  `/mnt/scratch/log/PE_server.log`) and the link has settled, it briefly resumes the interface
  so HDMI is set up again, then restarts the player.
- **Timing:** the firmware's SDL driver hands audio to the media server about 24 ms plus one
  buffer before it plays, on a steady schedule, but its callback wakes up a few ms late now and
  then. The player follows the steady schedule instead of the wake-up time, because the library
  treats any 5 ms jump as lost sync and corrects it audibly.
- **Rejoining:** the player always says goodbye with the reason `restart`, never `shutdown`. Music
  Assistant then keeps it in its group for a few minutes and puts it straight back into a running
  stream when it reconnects; after `shutdown` it would forget the group.
- **Finding Music Assistant:** the player dials the address it was set up with. Without one, or
  when that address hasn't answered for a minute (Music Assistant moved to a new address), it
  asks the network over mDNS for `_sendspin-server._tcp`, which Music Assistant announces, and
  switches to what it finds.
- **On the Steam Link**, the installed version lives in `/mnt/scratch/sendspin` plus the startup
  script in `/mnt/config/overlay/etc/init.d/startup/S99steam`. The USB version only uses RAM
  (`/tmp/sendspin`) and the stick.
- **The installers** (`app/`, `host/`) carry the player inside them, send it over SSH and run
  `device/ctl.sh` on the Steam Link.

## Building from source

On Linux, with cmake, git, Go 1.24+ and Python 3:

```sh
pip install ziglang
./build.sh            # Music Assistant 2.10.x (legacy protocol)
./build.sh encrypted  # Sendspin 1.0 servers
```

Everything lands in `dist/`. GitHub Actions (`.github/workflows/build.yml`) builds the same way
on every push. To publish a release, open **Actions > Build > Run workflow** and enter a version
such as `v1.0.1`; pushing a `v*` tag works too.

## Status

Tested on an original Steam Link with its final firmware and Music Assistant 2.10.6. Reports
from other units, firmware versions and setups are welcome: please open an issue with the
output of the `status` and `log` commands.

## License

Apache License 2.0, see [LICENSE](LICENSE).

The player and the installer apps include third-party code under its own licenses, among them
sendspin-cpp, micro-flac and micro-opus (Apache 2.0), Opus, IXWebSocket and Go's libraries
(BSD 3-Clause), and ArduinoJson (MIT). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Not affiliated with Valve, the Open Home Foundation or Music Assistant. Steam Link is a
trademark of Valve Corporation.
