# Roomy

A [Mumble](https://www.mumble.info) plugin for people who sit in the same room and are in the same voice chat.

Each microphone in the room also picks up everybody else in the room. The rest of the channel hears every sentence twice: once from the talker's own microphone and once, quieter and slightly later, from the neighbour's. Roomy removes the neighbour's voice from your microphone before it is sent.

## Status

Version 0.1 is untested on real microphones. The signal processing passes its automated tests on synthetic voices, and the network link passes its test on a single machine. Nobody has yet run it inside Mumble with two people in a room.

## Requirements

- Mumble 1.4 or newer, on Windows or Linux (64 bit)
- Everybody in the room installs the plugin
- All of their computers are on the same local network. A wired connection is best.
- Everybody wears headphones

People outside the room need nothing. The server needs nothing.

## Install

1. Download `roomy.mumble_plugin` from the [latest release](https://github.com/adman234/mumble-roomy/releases/latest). The same page has `roomy.dll` (Windows) and `libroomy.so` (Linux) on their own.
2. In Mumble open Configure, Settings, Plugins and choose Install plugin. Select the downloaded file.
3. Tick Enable next to Roomy.

**Do not double-click `roomy.mumble_plugin` to update an installed copy.** Mumble 1.5 crashes when it is started that way and the file would overwrite an existing plugin (the installer runs before Mumble has set up its plugin manager). A first install by double-click works. For updates use Install plugin inside Mumble, or close Mumble and copy `roomy.dll` into `%APPDATA%\Mumble\Mumble\Plugins` (Windows) or `~/.local/share/Mumble/Mumble/Plugins` (Linux), replacing the old file.

There is nothing to pair. When two people running the plugin are in the same channel and can reach each other on the local network, Mumble's log shows "Roomy: linked with (name)".

If Windows asks whether Mumble may accept network connections, allow it for private networks. The link usually also works if you decline, as both sides contact each other.

### Recommended Mumble setting

Mute each other locally (right-click the other person, Local Mute). You can hear them through the air, and hearing them again through the headphones a moment later is distracting. Roomy does not do this for you.

## Troubleshooting

Everybody in the room has to be connected to the **same Mumble server** and be in the **same channel**. The introduction between plugins travels through the server, so two people on different servers (say, one on the LAN address and one on the public address of a different server) never see each other. Using the local address or the public address of the same server both work, the audio itself always goes directly over the local network.

Roomy writes to Mumble's normal log window. After enabling the plugin you should see, in this order:

1. `Roomy: Roomy 0.1.2 is active, listening on UDP port ... Local addresses: ...` when the plugin is enabled. If this never appears, the plugin is not enabled (Configure, Settings, Plugins). If the addresses say "none found", the PC has no private network address.
2. `Roomy: looking for other Roomy users among the N other people in your channel.` after joining a channel. If N is 0, nobody else is there yet.
3. `Roomy: (name) is running Roomy, trying to reach them at ...` when the other person's plugin answered through the server. If this never appears while both are in the channel, the other plugin is not running or the server dropped the message.
4. `Roomy: linked with (name)` once the direct connection works. If instead you get `has not answered over the local network` after ten seconds (and again every thirty), the two PCs cannot reach each other. The message says how many probes were sent and how many packets arrived. If none arrived, check that both PCs are on the same network (not guest Wi-Fi, not a VPN) and that Windows does not treat the network as Public, which blocks incoming traffic. Set the network to Private (Settings, Network and internet, your connection, Network profile type) or add an inbound UDP allow rule for `mumble.exe`.

## How it works

1. **Finding each other.** On joining a channel the plugin tells the other users in it, through the Mumble server, which local network addresses and port it listens on. Clients without the plugin ignore this.
2. **Linking.** Plugins that learn of each other try to exchange packets directly. If that succeeds they are on the same network and a link is established. Only private network addresses are ever contacted.
3. **Reference audio.** While you transmit, your microphone signal also goes directly to every linked plugin, uncompressed, with almost no delay. This is the same audio the server gets anyway.
4. **Suppression.** Your own microphone signal is held back by 20 ms so that the others' audio is available first. Every 10 ms the plugin compares the spectrum of your microphone with that of the others. It learns how loud each of them typically is in your microphone while they talk alone, and then turns down those parts of the spectrum where their voice dominates.

In total your voice reaches the channel about 30 ms later than without the plugin.

## Limits

- **Talking at the same time.** While only the other person talks, their voice is removed almost completely. While both talk, it is only reduced, and your own voice may sound slightly thinner in those moments.
- **Muted neighbours.** The plugin only sees audio that Mumble is transmitting. If your neighbour is muted, or doesn't hold their push-to-talk key, but talks anyway, there is nothing to compare against and their voice stays in your microphone.
- **Loud bleed.** If the other person is nearly as loud in your microphone as you are yourself, the plugin can't tell the voices apart. Move the microphone closer to your mouth and lower its gain.
- **Up to four neighbours.**

## Settings

All settings are optional. Create `roomy.ini` in `%APPDATA%\Mumble` (Windows) or `~/.config/Mumble` (Linux) and restart Mumble:

```ini
# Set to false to switch the plugin off without uninstalling it
enabled = true

# How aggressively the other voice is removed, 0 to 12.
# Higher removes more of it but also damages your own voice more while both talk.
strength_db = 3

# The most any part of the signal is turned down
max_suppression_db = 30

# How long your microphone is held back to wait for the other microphone.
# Raise this in steps of 10 on Wi-Fi if the other voice comes through in bursts.
delay_ms = 20

# UDP port for the direct link, 0 picks a free one
port = 0
```

## Building

A C++17 compiler and CMake are needed. There are no other dependencies.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build --config Release
```

```bash
ctest --test-dir build -C Release --output-on-failure
```

The plugin is `build/Release/roomy.dll` on Windows and `build/libroomy.so` on Linux. To create an installable bundle, zip the library together with `manifest.xml` and name the archive `roomy.mumble_plugin`.

Pushing a tag such as `v0.1.0` makes the build workflow publish a release with the bundle attached. Keep the version in `CMakeLists.txt` and `manifest.xml` in sync with the tag.

## Third party

`third_party/mumble/MumblePlugin.h` is the plugin interface of Mumble, taken from its 1.5.x branch. It is covered by the license next to it.
