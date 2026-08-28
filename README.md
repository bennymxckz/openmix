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

Needs Visual Studio Build Tools with the C++ workload. Dear ImGui is fetched
by CMake, so the first configure needs a network connection.

    .	oolsuild.ps1            build
    .	oolsuild.ps1 -Run       build and start the mixer
    .	oolsuild.ps1 -Clean     reconfigure from scratch

CMake and Ninja ship inside Build Tools and are not on PATH; the script finds
them with vswhere, so no developer prompt is needed. From a developer prompt
the plain CMake commands work too:

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build

Produces two binaries: `openmix.exe`, the mixer window, and `openmix-cli.exe`,
a console build kept for debugging and headless use.

## Run

    build\openmix.exe

A mixer window with a level meter, volume fader and mute per channel, plus
pickers for your headphones and microphone. Changing either restarts only
that stream, so applications pointed at the openmix devices never notice. The
devices are plugged in at startup and unplug when openmix exits.

### Device names

Windows composes USB audio endpoint names as `<terminal type> (<product>)`
and a device cannot override that, so the channels appear as
`Speakers (Openmix - Game)`. Setting the endpoint's own friendly name is the
only thing that sticks, and it needs administrator rights -- once, because the
name persists:

    build\openmix-cli.exe --fix-names      (elevated, while openmix is running)

Closing the window or minimising it parks openmix in the tray with audio
still running. Left-click the tray icon to show or hide the mixer;
right-click for Quit. While hidden it stops rendering entirely.

### Console build

    build\openmix-cli.exe --out "HyperX Cloud Alpha S Game"
    build\openmix-cli.exe --list-devices        show output devices
    build\openmix-cli.exe --mic "Yeti"          pick the microphone source
    build\openmix-cli.exe --no-mic              skip the virtual microphone
    build\openmix-cli.exe --bus Game --bus Chat --bus Media --bus Browser
    build\openmix-cli.exe --loopback            tap apps instead (no devices)

Keys: `1`-`9` select bus, `+`/`-` gain, `m` mute, `q` quit.

## Setting it up

1. Run openmix with `--out` pointing at your headphones.
2. In Windows Settings -> System -> Sound -> Volume mixer, set each app's
   output device: Discord -> openmix Chat, Spotify -> openmix Media.
3. Leave your headphones as the system default so the monitor mix lands
   there, and so anything unassigned still reaches your ears.
4. In OBS, add each channel as an **Audio Input Capture** source on its own
   track -- they appear as `Openmix - Game`, `Openmix - Chat` and so on in the
   recording device list. Leave Media off the stream if you do not want
   Spotify recorded.

Each playback channel is a duplex device: applications render into the
playback side, and OBS records the same audio back from the capture side. The
two faders are independent, so you can drop game audio in your own ears
without touching what the stream hears, exactly as Sonar does.

## Status

Working: device creation and enumeration, USB/IP protocol (devlist, import,
control transfers, isochronous OUT), UAC1 descriptors with a feature unit for
host volume/mute, per-bus gain and mute, peak meters, monitor mixing, output
device selection, auto attach/detach, and a driverless loopback mode.

Settings live in `%APPDATA%\openmix\config.ini` as plain text -- device
choices, per-channel volume and mute -- written whenever you change something
rather than only at exit. "Start with Windows" registers a per-user HKCU Run
entry that launches openmix straight to the tray.

Not yet: per-bus EQ and microphone processing, an installer, and adaptive
resampling
to correct long-run clock drift between the USB packet clock and the render
device. Expect an occasional glitch on multi-hour sessions until that lands.

The microphone currently passes your input device through unprocessed and is
not folded into the monitor mix, so you will not hear yourself.

## License

MIT -- see [LICENSE](LICENSE). Fork it, ship it, make it yours.

Note for contributors: loading GPL or LGPL DSP plugins through a published
plugin ABI at the user's direction is fine. Vendoring their source into this
tree is not, and would change the licence of the whole project.
