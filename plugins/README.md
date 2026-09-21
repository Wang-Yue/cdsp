# ALSA Rate Notify Plugin for CamillaDSP & CDSP Studio

## Overview
This ALSA PCM I/O plugin (`ioplug`) bridges playback applications (e.g., Audacious, `mpv`, Spotify, web browsers) and **CamillaDSP / CDSP Studio** over an ALSA loopback device (`snd-aloop`).

When an application switches sample rates (e.g., between 44.1 kHz, 48 kHz, 96 kHz, 192 kHz) or sample formats (`FLOAT_LE`, `S16_LE`, `S24_LE`, `S32_LE`):
1. The plugin intercepts the `snd_pcm_hw_params()` request before configuring the slave device.
2. It automatically writes the new sample rate to the `"Capture Rate"` ALSA control on the `Loopback` card (device 1), triggering an immediate format/rate change event.
3. It configures the loopback playback endpoint (`hw:Loopback,0,0`) with the player's exact format and rate first, constraining the ALSA loopback cable so that when `cdsp` restarts with `format: null` (auto-detect), `cdsp` automatically and natively matches the player's format.
4. **`cdsp`'s ALSA capture backend** (`alsa_capture.c`) detects the `SND_CTL_EVENT_ELEM` control event and halts with `CDSP_STOP_REASON_CAPTURE_FORMAT_CHANGE`.
5. **`CDSP Studio`** (`MonitoringController.cpp`) catches the format change event and restarts the DSP engine at the new native sample rate and format.
6. The plugin performs a synchronized handshake with the active capture peer, ensuring bit-perfect playback from Frame 0 without initial audio drops or resampling.

---

## Prerequisites: Enabling ALSA Loopback (`snd-aloop`)

Before using this plugin, the Linux kernel ALSA loopback module (`snd-aloop`) must be loaded.

### 1. Load the Kernel Module (Current Session)
```bash
sudo modprobe snd-aloop
```

### 2. Make Persistent Across Reboots
To automatically load the loopback module on system boot:
```bash
echo "snd-aloop" | sudo tee /etc/modules-load.d/snd-aloop.conf
```

*(Optional)* If you want to specify module options (e.g. fixed card name or number of subdevices), create `/etc/modprobe.d/snd-aloop.conf`:
```text
options snd-aloop enable=1 index=0 pcm_substreams=8
```

### 3. Verify Loopback Sound Card
Check that the Loopback card is detected by ALSA:
```bash
cat /proc/asound/cards
# or
aplay -l | grep -i loopback
```
You should see output similar to:
```text
card 0 [Loopback       ]: Loopback - Loopback
```

---

## Build & Installation

```bash
cmake -B build
cmake --build build
cmake --install build/plugins
```
When installed via `cmake --install`, CMake automatically:
1. Installs `libasound_module_pcm_rate_notify.so` to your user ALSA plugin directory (`~/.local/lib/alsa-lib/`).
2. Installs `asoundrc` to `~/.asoundrc`.
3. Installs `50-cdsp.conf` to `~/.config/alsa/conf.d/50-cdsp.conf`.
4. Installs PipeWire & WirePlumber bit-perfect configurations to `~/.config/pipewire/` and `~/.config/wireplumber/`.
5. Automatically restarts active `pipewire`, `pipewire-pulse`, and `wireplumber` user services.

*(Even when executed via `sudo cmake --install`, CMake resolves `$SUDO_USER` to install configurations to the active user's home directory rather than `/root/`)*

**Zero Root Required**: Once installed, `"CDSP Studio / CamillaDSP Dynamic Rate Audio"` (`pcm.cdsp`) is automatically discovered by ALSA and immediately appears in device picker dropdowns across all applications (Audacious, VLC, mpv, etc.).

---

## Desktop & Application Audio Routing

There are two primary methods to route audio into **CDSP Studio**:

```text
                 [Music Player / Web Browser / Desktop Audio]
                                      │
          ┌───────────────────────────┴───────────────────────────┐
          ▼                                                       ▼
   Method 1: System-Wide Desktop Audio             Method 2: Direct Bit-Perfect ALSA
   PipeWire Desktop Sinks                          Dedicated Players (mpv, Audacious)
   "CDSP Studio (2ch / 8ch / 32ch)"                Select ALSA Device: "cdsp"
          │                                                       │
          └───────────────────────────┬───────────────────────────┘
                                      │
                                      ▼
      ┌───────────────────────────────────────────────────────────────┐
      │     ALSA Rate Notify Plugin (pcm.cdsp / pcm.cdsp_in)          │
      │     Writes dynamic sample rate to "Capture Rate" control      │
      └───────────────────────────────┬───────────────────────────────┘
                                      │
                                      ▼
      [hw:Loopback,0,0] (Kernel snd-aloop input)
                                      │ (virtual loopback cable)
                                      ▼
      [hw:Loopback,1,0] (ALSA Capture Backend in CDSP Studio)
                                      │
      [CDSP Studio / CamillaDSP Engine] (FIR/IIR Filters, EQ, Crossover, Volume)
                                      │
          ┌───────────────────────────┴───────────────────────────┐
          ▼                                                       ▼
   Playback Option A:                              Playback Option B:
   PipeWire Backend                                Direct ALSA Backend
   (Bit-Perfect via 50-bitperfect-dac.conf)        (Direct Hardware ALSA: hw:DAC)
   resample.disable = true                         Exclusive DAC access
          │                                                       │
          └───────────────────────────┬───────────────────────────┘
                                      ▼
                          [Physical DAC / Speakers]
```

### Method 1: System-Wide Desktop Audio (PipeWire)

Use this method for day-to-day desktop usage—including web browsers (YouTube, Netflix), Spotify, gaming, system notifications, and media players that use PipeWire or PulseAudio.

1. Open your desktop sound settings:
   - **GNOME:** Settings $\rightarrow$ Sound $\rightarrow$ Output Device
   - **KDE Plasma:** System Settings $\rightarrow$ Audio $\rightarrow$ Playback Devices
   - **CLI / Terminal:** `wpctl status` and `wpctl set-default <sink-id>`
2. Select your desired CDSP Studio sink endpoint:
   - **`CDSP Studio (Stereo 2ch)`** *(Default)*: Best for music listening, streaming services, web browsers, and general stereo audio.
   - **`CDSP Studio (Surround 8ch)`**: Best for 5.1 / 7.1 surround sound movies and games.
   - **`CDSP Studio (Pro Audio 32ch)`**: Best for multichannel DAW routing, multi-way active crossovers, and complex studio setups.

#### Dynamic Sample Rate Switching
When using any of these PipeWire desktop sinks:
- PipeWire's clock dynamically switches sample rates (44.1 kHz, 48 kHz, 88.2 kHz, 96 kHz, 176.4 kHz, 192 kHz, up to 768 kHz) to match the playing media stream.
- The underlying `pcm_rate_notify` plugin detects the rate transition and signals CDSP Studio to reload seamlessly without restarting desktop playback.
- Sinks not in use automatically suspend and release their ALSA subdevice.

---

### Method 2: Direct Bit-Perfect ALSA Output (Media Players)

Use this method for dedicated audiophile music players (e.g. **Audacious**, **DeaDBeeF**, **Strawberry**, **mpv**, or **VLC**) when you want bit-perfect, zero-resampling transmission that dynamically matches both the exact sample rate and channel count directly from the media file:

1. Open your player's Audio Preferences:
   - **Audacious:** Preferences $\rightarrow$ Audio $\rightarrow$ Output plugin: **ALSA Output** $\rightarrow$ Settings $\rightarrow$ PCM device: `cdsp` (or select `"CDSP Studio / CamillaDSP Dynamic Rate Audio"`).
   - **DeaDBeeF:** Preferences $\rightarrow$ Sound $\rightarrow$ Output plugin: **ALSA output** $\rightarrow$ Device: `cdsp`.
   - **Strawberry / Clementine:** Settings $\rightarrow$ Backend $\rightarrow$ Output: **ALSA** $\rightarrow$ Device: `cdsp`.
   - **mpv:** Add `--ao=alsa --audio-device=alsa/cdsp` to your command line or `~/.config/mpv/mpv.conf`.
2. In direct ALSA mode, the player bypasses the desktop sound server and communicates directly with `pcm_rate_notify.c`. The plugin dynamically passes through any channel count (1 to 32 channels) and sample rate without intermediate mixing.

---

## CDSP Studio Device Configuration

Configure the capture and playback endpoints in **CDSP Studio** under the **Device Settings** tab:

### 1. Capture Settings (Input from Applications)

| Setting | Recommended Value | Description |
| :--- | :--- | :--- |
| **Capture Backend** | **`ALSA`** | Direct Linux ALSA backend. |
| **Capture Device** | **`hw:Loopback,1,0`** *(or `hw:0,1,0`)* | Captures the output of the virtual loopback cable fed by `cdsp_in` (`hw:Loopback,0,0`). |
| **Capture Format** | **`AUTO`** *(or `S32_LE` / `F32_LE`)* | Matches the native sample format delivered across the loopback cable. |
| **Stop on Rate Change** | **`Enabled`** *(Checked)* | **Crucial:** Allows CamillaDSP to halt immediately upon detecting a rate change event on the loopback control, prompting CDSP Studio's `MonitoringController` to reload the DSP engine at the new sample rate. |

### 2. Playback Settings (Output to Hardware DAC)

Choose either of the two playback backends based on your setup:

#### Playback Option A: PipeWire Backend (Bit-Perfect to Physical DAC)
- **Playback Backend:** **`PipeWire`**
- **Playback Device:** Select your physical DAC or default PipeWire sink.
- **Why it is bit-perfect:** With the installed WirePlumber rule (`50-bitperfect-dac.conf`), PipeWire applies `resample.disable = true` and `audio.format = "S32LE"` directly to your hardware DAC. CDSP Studio streams audio directly to the DAC at its native sample rate without PipeWire software resampling or dither.
- **Advantages:** Hotplug-safe; allows device switching; does not lock the hardware ALSA device, allowing system alerts or other non-interfering audio to play if needed.

#### Playback Option B: Direct ALSA Backend (Hardware Direct)
- **Playback Backend:** **`ALSA`**
- **Playback Device:** Direct hardware address of your DAC (e.g. **`hw:DAC`**, **`hw:1,0`**, or **`hw:U18`**).
- **Advantages:** Lowest possible latency; completely bypasses user-space sound servers.
- **Note:** ALSA takes exclusive hardware access of the DAC. Other sound servers or applications cannot open the DAC directly while CDSP is running.

---

> [!CAUTION]
> ### ⚠️ Critical: Prevent Audio Feedback Loops
> **Never select `default`, `hw:Loopback,0,0`, or `cdsp_sink` as the CDSP Playback Device!**
> 
> `hw:Loopback,0,0` and `cdsp_sink` are the *input endpoints* where applications send audio into CDSP. Selecting them for playback will route CDSP's processed output back into its own input, creating an infinite, loud audio feedback loop.
> 
> **Always ensure the Playback Device points to your physical DAC or PipeWire output sink.**

---

## ALSA Configuration (Optional Customization)

`pcm.cdsp` is installed automatically to `~/.config/alsa/conf.d/50-cdsp.conf` and `~/.asoundrc`. 

If you want `cdsp` to be your system's global **`default`** fallback ALSA audio sink, add the following to `~/.asoundrc` or `/etc/asound.conf`:

```alsa
# Set CDSP Studio / cdsp as the global default playback PCM
pcm.!default {
    type plug
    slave.pcm "cdsp"
}

# Default mixer control
ctl.!default {
    type hw
    card "Loopback"
}
```

### User-Level Configuration (Custom Path)
If you installed the plugin to a custom user directory, declare the library path at the top of your `~/.asoundrc`:

```alsa
# Declare custom plugin library location (replace with your absolute path)
pcm_type.rate_notify {
    lib "/home/<username>/.local/lib/alsa-lib/libasound_module_pcm_rate_notify.so"
}

pcm.cdsp_in {
    type rate_notify
    slave "hw:Loopback,0,0"
    ctl_card "Loopback"
    ctl_device 1
    ctl_subdevice 0
}

pcm.!default {
    type plug
    slave.pcm "cdsp_in"
}

ctl.!default {
    type hw
    card "Loopback"
}
```

---

## PipeWire & WirePlumber User Configurations

If your system uses **PipeWire** (standard on modern Linux desktop environments like KDE Plasma or GNOME) alongside WirePlumber, bit-perfect audio configurations are included with the plugin and automatically installed to your `~/.config/` directory when running `cmake --install build/plugins`. CMake also automatically restarts the active user audio services upon installation.

### Configuration Overview

| Source File | Destination in `~/.config/` | Purpose |
| :--- | :--- | :--- |
| `plugins/pipewire/50-bitperfect-dac.conf` | `pipewire/pipewire.conf.d/50-bitperfect-dac.conf` | Configures global clock allowed sample rates (44.1 kHz – 768 kHz) and buffer quantum sizes. |
| `plugins/pipewire/60-cdsp-sink.conf` | `pipewire/pipewire.conf.d/60-cdsp-sink.conf` | Exposes the `cdsp` ALSA rate notify plugin as selectable Stereo (2ch), Surround (8ch), and Pro Audio (32ch) output sinks in GNOME and KDE Sound Settings. |
| `plugins/pipewire-pulse/99-bitperfect.conf` | `pipewire/pipewire-pulse.conf.d/99-bitperfect.conf` | Disables volume normalization and channel mixing in the PulseAudio layer; sets SoXR quality. |
| `plugins/wireplumber/50-bitperfect-dac.conf` | `wireplumber/wireplumber.conf.d/50-bitperfect-dac.conf` | Dynamic rule matching USB DACs (`~alsa_output.usb-.*`), setting `resample.disable = true` and `audio.format = "S32LE"`. Hotplug-safe. |
| `plugins/wireplumber/50-loopback.conf` | `wireplumber/wireplumber.conf.d/50-loopback.conf` | Disables the raw ALSA Loopback device (`snd_aloop`) from generating duplicate output sinks, ensuring only the dynamic rate sink appears in desktop settings. |

---

### Customizing Output Devices & Formats (Optional)

By default, `50-bitperfect-dac.conf` targets any connected USB audio device (`~alsa_output.usb-.*`). If you want to use a different output device—such as an onboard/PCI soundcard, HDMI audio, or a specific USB DAC model—follow these steps:

#### 1. Identify Your Output Device's Node Name
Run `wpctl status` to find your target device under `Audio -> Sinks`, then inspect its node name:
```bash
wpctl status
# Inspect using the sink ID (e.g., 45):
wpctl inspect <sink-id> | grep 'node.name'
```

Common node patterns:
- **Any USB DAC (Default):** `~alsa_output.usb-.*`
- **Specific USB DAC:** `~alsa_output.usb-Topping_.*` *(recommended if you also have a USB headset or microphone connected)*
- **Internal / Onboard (PCI/PCIe) Soundcard:** `~alsa_output.pci-.*analog-stereo`
- **HDMI / DisplayPort Audio:** `~alsa_output.pci-.*hdmi-stereo`
- **Exact Node Name:** `"alsa_output.pci-0000_00_1f.3.analog-stereo"`

#### 2. Query Supported Rates and Formats
Check your soundcard's hardware-supported formats and sample rates:
```bash
# For USB DACs:
cat /proc/asound/cardX/stream0

# For PCI / Onboard Soundcards:
cat /proc/asound/cardX/codec#* | grep -E "rates|formats"
```
*(Find the card number `X` using `cat /proc/asound/cards`)*

#### 3. Update WirePlumber Configuration
Edit `~/.config/wireplumber/wireplumber.conf.d/50-bitperfect-dac.conf`:
- Set `node.name` to match your device pattern (e.g., `~alsa_output.pci-.*analog-stereo`).
- Adjust `audio.format` if your hardware accepts a different format (e.g. `"S16LE"`, `"S24LE"`, or `"S32LE"`).

#### 4. Update PipeWire Allowed Rates
Edit `~/.config/pipewire/pipewire.conf.d/50-bitperfect-dac.conf`:
- Adjust `default.clock.allowed-rates` to contain only the rates supported by your device (e.g., `[ 44100 48000 88200 96000 192000 ]`).

#### 5. Apply Changes
Restart the audio services to reload the modified configuration:
```bash
systemctl --user restart pipewire pipewire-pulse wireplumber
```

---

## Verification Commands

```bash
# 1. Verify active access mode on the loopback playback endpoint (should show: access: MMAP_INTERLEAVED)
cat /proc/asound/Loopback/pcm0p/sub0/hw_params

# 2. Test sample rate switching with aplay
aplay -f S16_LE -r 44100 -c 2 test44.wav
aplay -f S16_LE -r 96000 -c 2 test96.wav

# 3. Check that the ALSA loopback control is updated on rate change
amixer -c Loopback cget iface=PCM,name='Capture Rate',device=1,subdevice=0

# 4. Check active audio devices and default sink (* indicates active default)
wpctl status

# 5. Inspect sink properties to ensure resample.disable and format are active
wpctl inspect <sink-node-id>

# 6. Monitor real-time sample rate, quantum, and resampler status during playback
pw-top
```
