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

## ALSA Configuration (Optional Customization)

If you installed system-wide (Option 1), `pcm.cdsp` works automatically out of the box. 

If you want `cdsp` to be your system's global **`default`** audio sink, or if you did a user-level install (Option 2), add the following to `~/.asoundrc` or `/etc/asound.conf`:

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
If you installed the plugin to a user directory instead of the system directory, declare the library path at the top of your `~/.asoundrc`:

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

## CDSP Studio Device Configuration

In **CDSP Studio** $\rightarrow$ **Device Settings** tab:

| Setting | Recommended Value | Description |
| :--- | :--- | :--- |
| **Capture Backend** | `ALSA` | Linux ALSA backend |
| **Capture Device** | **`hw:Loopback,1,0`** *(or `hw:0,1,0`)* | Captures the loopback output originating from `cdsp_in` (`hw:Loopback,0,0`). |
| **Capture Format** | `F32_LE` *(or `AUTO`)* | Native sample format. |
| **Stop on Rate Change** | **`Enabled`** | Required to allow CDSP Studio to restart the DSP engine on sample rate change events. |
| **Playback Backend** | `ALSA` or `PipeWire` | Output backend. |
| **Playback Device** | **Your physical DAC** (e.g. `hw:DAC`, `pipewire`, `pulse`) | ⚠️ **Never select `default` or `hw:Loopback,0,0` for Playback**, as that is the input channel reserved for playback applications. If testing in a virtual environment without a DAC, select `hw:Loopback,0,1` (Device 0, Subdevice 1). |

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
