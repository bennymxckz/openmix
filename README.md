# openmix

An open-source per-application audio mixer for Windows, in the spirit of
SteelSeries Sonar but without the bundled clipping software.

openmix publishes its own playback devices -- **openmix Game**, **openmix
Chat**, **openmix Media** -- plus a virtual microphone, **openmix Mic**, that
any app can select in Windows sound settings. It mixes them with independent gain and sends the monitor mix to
your real headphones.

## How it works, and why that matters

Windows has no user-mode API for creating an audio endpoint: endpoints are
minted by `AudioEndpointBuilder` only from kernel-mode drivers. That single
fact is what killed every previous open-source attempt at this, because
shipping a kernel driver means Microsoft attestation signing, an EV
certificate and a registered legal entity.

openmix sidesteps it entirely. It acts as a **USB/IP server exporting virtual
USB Audio Class 1.0 devices**. The already-Microsoft-signed `usbip-win2` VHCI
driver attaches them, and Windows' in-box `usbaudio.sys` publishes the
endpoints -- named from each device's USB product string. Audio arrives as
isochronous USB packets straight into the engine.

No kernel code of our own, nothing to sign, Secure Boot stays on, and
anti-cheat is untouched.

## Requirements

- Windows 10 2004+ / Windows 11
- [usbip-win2](https://github.com/vadimgrn/usbip-win2) **v0.9.7.7**
  (v0.9.7.8 has a known BSOD bug -- avoid it)

## Build

MSVC + Windows SDK, from a developer prompt:

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build

## Run

    build\openmix.exe --out "HyperX Cloud Alpha S Game"

The three devices are plugged in automatically at startup and unplug when
openmix exits. `--out` picks where the monitor mix goes; openmix's own
devices are never selectable as the output, so it cannot feed back into
itself.

    build\openmix.exe --list-devices        show output devices
    build\openmix.exe --mic "Yeti"          pick the microphone source
    build\openmix.exe --no-mic              skip the virtual microphone
    build\openmix.exe --bus Game --bus Chat --bus Media --bus Browser
    build\openmix.exe --loopback            tap apps instead (no devices)

Keys: `1`-`9` select bus, `+`/`-` gain, `m` mute, `q` quit.
A `*` after a bus name means its device is not currently attached.

## Setting it up

1. Run openmix with `--out` pointing at your headphones.
2. In Windows Settings -> System -> Sound -> Volume mixer, set each app's
   output device: Discord -> openmix Chat, Spotify -> openmix Media.
3. Leave your headphones as the system default so the monitor mix lands
   there, and so anything unassigned still reaches your ears.
4. In OBS, add each openmix device as an Audio Input Capture on its own
   track. Leave Media off the stream if you do not want Spotify recorded.

## Status

Working: device creation and enumeration, USB/IP protocol (devlist, import,
control transfers, isochronous OUT), UAC1 descriptors with a feature unit for
host volume/mute, per-bus gain and mute, peak meters, monitor mixing, output
device selection, auto attach/detach, and a driverless loopback mode.

Not yet: per-bus EQ and microphone processing, a GUI, and adaptive resampling
to correct long-run clock drift between the USB packet clock and the render
device. Expect an occasional glitch on multi-hour sessions until that lands.

The microphone currently passes your input device through unprocessed and is
not folded into the monitor mix, so you will not hear yourself.

## License

MIT -- see [LICENSE](LICENSE). Fork it, ship it, make it yours.

Note for contributors: loading GPL or LGPL DSP plugins through a published
plugin ABI at the user's direction is fine. Vendoring their source into this
tree is not, and would change the licence of the whole project.
