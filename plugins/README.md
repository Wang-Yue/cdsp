# ALSA Rate Notify Plugin for CamillaDSP & Monitor-Qt

## Overview
This ALSA PCM I/O plugin (`ioplug`) bridges playback applications (e.g., Audacious, `mpv`, Spotify, web browsers) and **CamillaDSP / Monitor-Qt** over an ALSA loopback device (`snd-aloop`).

When an application switches sample rates (e.g., between 44.1 kHz, 48 kHz, 96 kHz, 192 kHz) or sample formats (`FLOAT_LE`, `S16_LE`, `S24_LE`, `S32_LE`):
1. The plugin intercepts the `snd_pcm_hw_params()` request before configuring the slave device.
2. It automatically writes the new sample rate to the `"Capture Rate"` ALSA control on the `Loopback` card (device 1), triggering an immediate format/rate change event.
3. It configures the loopback playback endpoint (`hw:Loopback,0,0`) with the player's exact format and rate first, constraining the ALSA loopback cable so that when `cdsp` restarts with `format: null` (auto-detect), `cdsp` automatically and natively matches the player's format.
4. **`cdsp`'s ALSA capture backend** (`alsa_capture.c`) detects the `SND_CTL_EVENT_ELEM` control event and halts with `CDSP_STOP_REASON_CAPTURE_FORMAT_CHANGE`.
5. **`Monitor-Qt`** (`MonitoringController.cpp`) catches the format change event and restarts the DSP engine at the new native sample rate and format.
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

### Option 1: System-Wide Installation (Recommended - Zero User Config Needed)
```bash
cd Monitor-Qt/build
sudo cmake --install plugins/alsa_rate_notify
```
When installed via `cmake --install`, CMake automatically installs:
1. `libasound_module_pcm_rate_notify.so` to your distro's ALSA plugin directory (e.g. `/usr/lib/x86_64-linux-gnu/alsa-lib/` or `/usr/lib64/alsa-lib/`).
2. `50-cdsp.conf` to `/etc/alsa/conf.d/50-cdsp.conf`.

**Zero Configuration Required**: Once installed, `"Monitor-Qt / CamillaDSP Dynamic Rate Audio"` (`pcm.cdsp`) is automatically discovered by ALSA and immediately appears in device picker dropdowns across all applications (Audacious, VLC, mpv, etc.) without creating or editing `~/.asoundrc`.

### Option 2: User-Level Installation (No Root/Sudo Required)
```bash
# Build the plugin
cd Monitor-Qt
cmake -B build
cmake --build build

# Copy to user directory
mkdir -p ~/.local/lib/alsa-lib
cp build/plugins/alsa_rate_notify/libasound_module_pcm_rate_notify.so ~/.local/lib/alsa-lib/
```

---

## ALSA Configuration (Optional Customization)

If you installed system-wide (Option 1), `pcm.cdsp` works automatically out of the box. 

If you want `cdsp` to be your system's global **`default`** audio sink, or if you did a user-level install (Option 2), add the following to `~/.asoundrc` or `/etc/asound.conf`:

```alsa
# Set Monitor-Qt / cdsp as the global default playback PCM
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

## Monitor-Qt Device Configuration

In **Monitor-Qt** $\rightarrow$ **Device Settings** tab:

| Setting | Recommended Value | Description |
| :--- | :--- | :--- |
| **Capture Backend** | `ALSA` | Linux ALSA backend |
| **Capture Device** | **`hw:Loopback,1,0`** *(or `hw:0,1,0`)* | Captures the loopback output originating from `cdsp_in` (`hw:Loopback,0,0`). |
| **Capture Format** | `F32_LE` *(or `AUTO`)* | Native sample format. |
| **Stop on Rate Change** | **`Enabled`** | Required to allow Monitor-Qt to restart the DSP engine on sample rate change events. |
| **Playback Backend** | `ALSA` or `PipeWire` | Output backend. |
| **Playback Device** | **Your physical DAC** (e.g. `hw:DAC`, `pipewire`, `pulse`) | ⚠️ **Never select `default` or `hw:Loopback,0,0` for Playback**, as that is the input channel reserved for playback applications. If testing in a virtual environment without a DAC, select `hw:Loopback,0,1` (Device 0, Subdevice 1). |

---

## Optional: PipeWire / WirePlumber Loopback Configuration

If your system uses **PipeWire** (standard on modern Linux desktop environments like KDE Plasma or GNOME) and you route system/desktop audio to the **Loopback** sink:

### Why this is needed
By default, PipeWire opens ALSA PCM sinks using planar (non-interleaved) access mode (`MMAP_NONINTERLEAVED`, format `S32P`). However, the Linux kernel's `snd-aloop` driver strictly requires matching interleaved modes on both ends of the loopback cable. When CamillaDSP attempts to capture using standard interleaved access (`MMAP_INTERLEAVED`), the kernel rejects the capture stream with:
```text
Capture error: ALSA function 'snd_pcm_start' failed with error 'I/O error (5)'
```

### Configuration
Force WirePlumber to open the ALSA Loopback device with interleaved audio format (`S32LE`).

Create `~/.config/wireplumber/wireplumber.conf.d/50-loopback.conf` (or `/etc/wireplumber/wireplumber.conf.d/50-loopback.conf` for system-wide):

```spa-json
monitor.alsa.rules = [
  {
    matches = [
      {
        node.name = "~alsa_output.*snd_aloop.*"
      }
    ]
    actions = {
      update-props = {
        audio.format = "S32LE"
      }
    }
  }
]
```

Apply the changes by restarting WirePlumber:
```bash
systemctl --user restart wireplumber
```

You can verify the active access mode on the loopback playback endpoint:
```bash
cat /proc/asound/Loopback/pcm0p/sub0/hw_params
# Should show: access: MMAP_INTERLEAVED
```

---

## Verification

You can test rate switching by playing different sample rate files with `aplay` or any media player:

```bash
# Play 44.1 kHz
aplay -f S16_LE -r 44100 -c 2 test44.wav

# Play 96 kHz
aplay -f S16_LE -r 96000 -c 2 test96.wav
```

Check that the ALSA loopback control is updated:
```bash
amixer -c Loopback cget iface=PCM,name='Capture Rate',device=1,subdevice=0
```
