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

## PipeWire & WirePlumber User Configurations

If your system uses **PipeWire** (standard on modern Linux desktop environments like KDE Plasma or GNOME) alongside WirePlumber, configure the following user configuration files under `~/.config/` for bit-perfect audio playback, dynamic USB DAC rate switching, hotplug safety, and ALSA loopback compatibility with CamillaDSP.

### Directory Structure

```text
~/.config/
├── pipewire/
│   ├── pipewire.conf.d/
│   │   └── 50-bitperfect-dac.conf
│   └── pipewire-pulse.conf.d/
│       └── 99-bitperfect.conf
└── wireplumber/
    └── wireplumber.conf.d/
        ├── 50-bitperfect-dac.conf
        └── 50-loopback.conf
```

---

### Step 1: Identify Your USB DAC Hardware Capabilities

Before creating the configurations, query your DAC's hardware capabilities:

1. **Find your DAC's ALSA card number:**
   ```bash
   cat /proc/asound/cards
   ```
   *(Note the card number `X` corresponding to your USB DAC)*

2. **Check supported sample rates & formats:**
   ```bash
   cat /proc/asound/cardX/stream0
   ```
   * **Rates:** Look at the `Rates:` line (e.g. `44100, 48000, 88200, 96000, 176400, 192000...`).
   * **Format:** Look at the `Format:` line under `Playback` (e.g. `S32_LE`, `S24_3LE`, `S16_LE`). Most modern DACs accept `S32LE`.

3. **Find your DAC's WirePlumber node name:**
   ```bash
   wpctl status
   # Locate your DAC sink ID, then inspect it:
   wpctl inspect <sink-id> | grep 'node.name'
   ```
   *(Example: `node.name = "alsa_output.usb-Manufacturer_Model_Serial-00.analog-stereo"`)*

---

### Step 2: Configuration Files

#### 1. PipeWire Global Clock & Allowed Sample Rates
**File:** `~/.config/pipewire/pipewire.conf.d/50-bitperfect-dac.conf`

**Role:** Defines the fallback rate and list of sample rates PipeWire may negotiate with your DAC. When resampling is disabled on the sink, PipeWire dynamically switches the hardware clock rate to match the source file.

```spa-json
# =============================================================
# Bit-Perfect Audio Configuration for PipeWire
# Global clock and allowed rates
# =============================================================

context.properties = {
    # Fallback rate when idle — NOT fixed playback rate.
    default.clock.rate          = 48000

    # Rates PipeWire may negotiate with the DAC.
    # Set this array to the sample rates supported by your DAC from stream0:
    default.clock.allowed-rates = [ 44100 48000 88200 96000 176400 192000 352800 384000 705600 768000 ]

    # Buffer quantum settings (1024 ~= 23ms at 44.1kHz)
    default.clock.quantum       = 1024
    default.clock.min-quantum   = 32
    default.clock.max-quantum   = 8192
}
```

---

#### 2. PipeWire-Pulse Compatibility Layer Tuning
**File:** `~/.config/pipewire/pipewire-pulse.conf.d/99-bitperfect.conf`

**Role:** Ensures applications using the PulseAudio API output bit-perfect streams without digital volume attenuation, channel mixing, or unnecessary resampling.

```spa-json
# =============================================================
# PipeWire-Pulse Tuning for Bit-Perfect Playback
# =============================================================

pulse.properties = {
    pulse.default.format = F32
}

stream.properties = {
    # Maximum SoXR resampling quality used ONLY as fallback
    # (e.g. when multiple streams with different rates play simultaneously)
    resample.quality = 15

    # Disable channel mixing modifications
    channelmix.normalize    = false
    channelmix.upmix        = false
    channelmix.upmix-method = none
    channelmix.mix-lfe      = false
}
```

---

#### 3. WirePlumber Dynamic USB DAC Rule (Hotplug Safe)
**File:** `~/.config/wireplumber/wireplumber.conf.d/50-bitperfect-dac.conf`

**Role:** Dynamically matches your USB DAC whenever connected, disables PipeWire's resampler (`resample.disable = true`), sets the hardware container format (`audio.format`), and assigns high priority (`9000`). 

> **Important**: Applying these properties dynamically via WirePlumber rules (rather than a static `context.objects` sink in `pipewire.conf`) ensures that detaching the USB cable cleanly removes the audio node and falls back to onboard audio without freezing or crashing PipeWire.

```spa-json
# =============================================================
# WirePlumber Dynamic Bit-Perfect Rule for USB DAC
# =============================================================

monitor.alsa.rules = [
  {
    matches = [
      {
        # Match pattern for your USB DAC node name from `wpctl inspect`.
        # You can use a specific pattern (e.g., "~alsa_output.usb-My_DAC_Name_.*")
        # or match any USB audio output: "~alsa_output.usb-.*"
        node.name = "~alsa_output.usb-Topping_DX3_Pro_.*"
      }
    ]
    actions = {
      update-props = {
        # Set to the widest format your DAC accepts from stream0 (typically S32LE, S24LE, S24_3LE, or S16LE)
        audio.format          = "S32LE"

        # Disable PipeWire sample rate conversion (passes native stream rate to DAC)
        resample.disable      = true

        # Hardware buffer tuning
        api.alsa.period-size  = 1024
        api.alsa.headroom     = 0

        # Prioritize USB DAC over internal soundcards when plugged in
        priority.driver       = 9000
        priority.session      = 9000
      }
    }
  }
]
```

---

#### 4. WirePlumber ALSA Loopback Interleaved Format
**File:** `~/.config/wireplumber/wireplumber.conf.d/50-loopback.conf`

**Role:** Forces WirePlumber to open the ALSA Loopback device (`snd_aloop`) with interleaved audio format (`S32LE`).

**Why this is needed:** By default, PipeWire opens ALSA PCM sinks using planar (non-interleaved) access mode (`MMAP_NONINTERLEAVED`, format `S32P`). However, the Linux kernel's `snd-aloop` driver strictly requires matching interleaved modes on both ends of the loopback cable. When CamillaDSP attempts to capture using standard interleaved access (`MMAP_INTERLEAVED`), the kernel rejects the capture stream with:
```text
Capture error: ALSA function 'snd_pcm_start' failed with error 'I/O error (5)'
```

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

---

## Service Management & Verification

### Restart Audio Services
```bash
systemctl --user restart pipewire pipewire-pulse wireplumber
```

### Verification Commands

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
