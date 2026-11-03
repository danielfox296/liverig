# LiveRig on iPhone

The iPhone app is the same LiveRig as the desktop: same engine, presets,
looper, tuner and foot controller, with a phone layout and without the
backing-track side (songs, stems, splitter). It runs natively, so latency is
whatever your interface and iOS buffer give you, typically 5 to 10 ms round
trip.

## Install on your iPhone (free Apple ID)

You need a Mac with Xcode. A free Apple ID works; the app then has to be
reinstalled from the Mac every 7 days (Apple's rule for free accounts).

### One-time setup

1. **Install Xcode** from the Mac App Store. Open it once, accept the licence,
   and let it install the iOS platform when it asks.
2. **Add your Apple ID**: Xcode > Settings > Accounts > **+** > Apple ID.
3. **Turn on Developer Mode on the iPhone**: plug it into the Mac, then on the
   phone go to Settings > Privacy & Security > Developer Mode, turn it on, and
   let it restart. (The option appears after the phone has been connected to
   Xcode once.)

### Build and run

In Terminal, from your LiveRig folder:

```bash
git fetch origin claude/happy-edison-qha9oo
```

```bash
git checkout claude/happy-edison-qha9oo
```

```bash
cmake -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS
```

```bash
open build-ios/LiveRig.xcodeproj
```

Then in Xcode:

1. At the top, pick the **LiveRig** scheme and your **iPhone** as the
   destination.
2. In the left sidebar click the blue **LiveRig** project, select the
   **LiveRig** target, open **Signing & Capabilities**, and set **Team** to
   *your name (Personal Team)*. If Xcode says the bundle identifier is taken,
   change it to something unique like `com.yourname.liverig`.
3. Press **Run** (the play button).

The first time, the phone refuses to open an app from an unknown developer.
On the phone: Settings > General > VPN & Device Management > your Apple ID >
**Trust**. Then open LiveRig from the home screen.

### Every 7 days

Plug the phone in, open the project, press **Run**. Your settings and presets
are kept.

## Plugging in

- iPhone 12 and older (Lightning): use Apple's **Lightning to USB 3 Camera
  Adapter** (the one with a charging port) for most interfaces.
- Bass into input 1, headphones or monitors on the interface outputs.
- The line under the board shows the real figures: sample rate, buffer size,
  I/O latency and the chain's own latency.

## What is different from the desktop

- No songs, stems or splitter (by design).
- Hosted plugins are iOS AUv3 plugins: the Pedal stage, the Keys instrument,
  and the Autotune/Vocoder lanes find whatever AUv3 apps are installed.
- Captures (.nam) are imported with the Files picker. The app's folder also
  shows up in the Files app and in Finder when the phone is connected.

## Building without a phone

GitHub builds the iOS app on every push (`.github/workflows/ios.yml`),
unsigned, to catch build breaks. On Linux, `rig_uishot <dir> --phone` renders
the phone layout to PNGs at iPhone 12 size.
