# moonglide-chiaki-tap

A small headless PS5 Remote Play client on top of chiaki-ng's `libchiaki`. It writes every
compressed video frame into POSIX shared memory (`/dev/shm/moonglide-remoteplay`, layout in
`rp_tap.h`) for moonglide, plays the sound through PulseAudio (optionally held back so it
meets the picture) and forwards SDL game controllers to the console.

It is a separate process because libchiaki is AGPL-3.0 (with the OpenSSL exception) and moonglide
is MIT. This directory is AGPL; see `LICENSE`. Only `rp_tap.h` (MIT) is shared.

## Build

    ./build.sh

Builds chiaki-ng v1.10.1 (library only) into `~/.cache/moonglide/chiaki-tap/` and installs
`~/.local/share/moonglide/bin/moonglide-chiaki-tap`. Needs git, cmake, ninja, curl, unzip, a C
compiler and the openssl/opus/pulse dev headers; libevent-dev and libsdl2-dev are unpacked
privately if missing (`apt-get download`, no root).

## Run

    moonglide-chiaki-tap --host 192.168.1.x [--wake] [--reconnect] [--codec h265] [--res 720|1080|1440|2160] [--fps 60]

The registration is picked by discovering the console at `--host` and matching its host-id (the
`server_mac`) or name against, in this order, moonglide's own conf
`~/.config/moonglide/remoteplay.conf` and then the Chiaki GUI's `~/.config/Chiaki/Chiaki.conf`
(`--app-conf`, `--conf`). `--nickname NAME` forces a registration by name; with no match the
first usable one is taken. `--res` is 720, 1080, 1440 or 2160 (libchiaki has presets up to 1080p;
1440/2160 send a custom size and the console may downgrade it, untested), `--fps` 30 or 60.
Keys are never logged. Other options: `--shm`, `--bitrate KBPS`, `--no-audio`,
`--no-controller`, `-v`. Test helpers: `--discover-only` (passive UDP status query),
`--check-conf` (prints only field lengths).

## DualSense / DualSense Edge

When a DualSense is attached at connect time the session starts with `enable_dualsense` and the
console sends the full DualSense feedback; ported from chiaki-ng v1.10.1:

- SDL HIDAPI PS4/PS5 drivers and their rumble hints are on (rumble over Bluetooth needs this).
- Rumble, adaptive triggers (`CHIAKI_EVENT_TRIGGER_EFFECTS`) and the mic LED go through
  `SDL_GameControllerSendEffect` with the DualSense output report; the console's haptic/trigger
  intensity settings are honoured. Lightbar colour and player index are applied via SDL.
- Haptics: over USB the pad's 4-channel audio device ("DualSense", channels 3/4) is opened through
  SDL audio (PipeWire preferred) and fed the haptic stream. Over Bluetooth there is no audio device
  (same in chiaki-ng), so the haptic stream is turned into rumble ("haptics to rumble").
- Gyro/accel (with chiaki's orientation tracker and the console's motion reset) and the touchpad
  (two fingers) are sent in the controller state. Other pads (DS4, Xbox, ...) get rumble, motion and
  touchpad where SDL provides them; only the DualSense-specific parts are skipped.
- The mic LED shows "muted" (this client sends no microphone). Speaker/headset audio on the pad is not
  supported by chiaki-ng v1.10.1 either.
- A DualSense plugged in after the session started is used for input, but the console only sends
  DualSense feedback when one was present at connect time.

## Setup modes (no chiaki-ng needed)

Each prints one JSON object per line on stdout, logs on stderr, exits 0 on success and 1 on
failure (`{"ok":false,"error":"..."}`).

| mode | output |
|---|---|
| `--discover-all [--timeout-ms N]` (2000) | per console: `{host,name,type,state:"ready"\|"standby",system_version,host_id,registered}` (broadcast on every interface, PS5 and PS4 ports) |
| `--psn-login-url` | `{url}`: open it in a browser and sign in |
| `--psn-account-id REDIRECT_URL` | `{ok,account_id,online_id}`: paste the address the browser was redirected to; the code is exchanged for the account id (base64, 8 bytes little endian, as chiaki-ng stores it) |
| `--register --host IP --psn-account-id B64 --pin NNNNNNNN|- [--ps4]` (`--pin -` reads the PIN from stdin) | `{ok,name,type,host_id}`; the registration goes to `~/.config/moonglide/remoteplay.conf` (mode 0600, same QSettings layout as Chiaki.conf, written atomically). The console must show its Link Device screen with the PIN. PS4 registers as 10.x with the account id (PS4 < 7.0 needs an online id, not supported) |
| `--list-registered` | per registration: `{name,type,host_id,source:"moonglide"\|"chiaki-ng"}` |

`host_id` is the console's MAC as 12 lowercase hex digits. Keys, PIN, OAuth codes and tokens are
never printed or logged (use `--pin -` to keep the PIN out of the process list).

## Shm semantics

The shm file is created at start (state IDLE, magic 0). A stream starts at the first video sample
of a session (chiaki delivers the codec header NAL units first): fields are filled, `session`
bumped, `write_seq` = 0, codec header stored, `magic` set last, state STREAMING. `CONNECTED` only
updates `status`. On session end `magic` = 0 and state QUIT with the reason in `status`. The file
is left in place on exit.

## rp_tap.h

`rp_tap.h` here is a copy of moonglide's `src/rp_tap.h`; moonglide's copy is the source of truth.
Re-copy it when the layout changes.
