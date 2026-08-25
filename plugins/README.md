# ALSA Rate Notify Plugin for CamillaDSP & Monitor-Qt

## Overview
This ALSA PCM I/O plugin (`ioplug`) bridges playback applications (e.g., Audacious, `mpv`, Spotify, web browsers) and **CamillaDSP / Monitor-Qt** over an ALSA loopback device (`snd-aloop`).

When an application switches sample rates (e.g., between 44.1 kHz, 48 kHz, 96 kHz, 192 kHz):
1. The plugin intercepts the `snd_pcm_hw_params()` request before configuring the slave device.
2. It automatically writes the new sample rate to the `"Capture Rate"` ALSA control on the `Loopback` card (device 1), creating the control if it doesn't already exist.
3. **`cdsp`'s ALSA capture backend** (`alsa_capture.c`) detects the `SND_CTL_EVENT_ELEM` control event and halts with `CDSP_STOP_REASON_CAPTURE_FORMAT_CHANGE`.
4. **`Monitor-Qt`** (`MonitoringController.cpp`) catches the format change event and automatically restarts the DSP engine at the new native sample rate.
5. The plugin completes opening the loopback playback side (`hw:Loopback,0,0`) at the matching sample rate, ensuring bit-perfect playback without resampling.

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

### Option 1: System-Wide Installation (Recommended)
```bash
cd Monitor-Qt/build
sudo cmake --install plugins/alsa_rate_notify

# Or copy manually to your architecture's ALSA plugins directory:
sudo cp plugins/alsa_rate_notify/libasound_module_pcm_rate_notify.so $(pkg-config --variable=libdir alsa)/alsa-lib/
```

### Option 2: User-Level Installation (No Root/Sudo Required)
```bash
# Build the plugin
cd Monitor-Qt
cmake -B build
cmake --build build

# Copy to a user directory (e.g. ~/.local/lib/alsa-lib)
mkdir -p ~/.local/lib/alsa-lib
cp build/plugins/alsa_rate_notify/libasound_module_pcm_rate_notify.so ~/.local/lib/alsa-lib/
```

---

## ALSA Configuration (`~/.asoundrc` or `/etc/asound.conf`)

### Standard Configuration (System Install)
When installed to the system ALSA plugins directory, ALSA automatically detects the plugin on any architecture:

```alsa
# Rate notify wrapper over Loopback playback device (Device 0, Subdevice 0)
pcm.cdsp_in {
    type rate_notify
    slave "hw:Loopback,0,0"
    ctl_card "Loopback"
    ctl_device 1
    ctl_subdevice 0
}

# Set as default playback PCM for desktop applications
pcm.!default {
    type plug
    slave.pcm "cdsp_in"
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
