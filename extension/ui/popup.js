/**
 * @file popup.js
 * @brief Extension Popup UI logic, Real-time Spectrum Visualizer, and JSON Config Editor.
 */

// Preset definitions conforming to CDSP schema
const PRESETS = {
  flat: {
    title: "Flat Direct Bypass",
    devices: { samplerate: 48000, chunksize: 128, capture: { type: "WebAudio", channels: 2 }, playback: { type: "WebAudio", channels: 2 } },
    filters: {},
    pipeline: []
  },
  bass_boost: {
    title: "Bass Shelf (+4.5 dB)",
    devices: { samplerate: 48000, chunksize: 128, capture: { type: "WebAudio", channels: 2 }, playback: { type: "WebAudio", channels: 2 } },
    filters: {
      bass_shelf: { type: "Biquad", parameters: { type: "Lowshelf", freq: 105.0, gain: 4.5, q: 0.707 } },
      pre_gain: { type: "Gain", parameters: { gain: -4.5, inverted: false } }
    },
    pipeline: [
      { type: "Filter", channels: [0], names: ["pre_gain", "bass_shelf"] },
      { type: "Filter", channels: [1], names: ["pre_gain", "bass_shelf"] }
    ]
  },
  harman_target: {
    title: "Harman Target EQ",
    devices: { samplerate: 48000, chunksize: 128, capture: { type: "WebAudio", channels: 2 }, playback: { type: "WebAudio", channels: 2 } },
    filters: {
      sub_bass: { type: "Biquad", parameters: { type: "Lowshelf", freq: 80.0, gain: 5.0, q: 0.71 } },
      mid_dip: { type: "Biquad", parameters: { type: "Peaking", freq: 2500.0, gain: -2.0, q: 1.8 } },
      treble_air: { type: "Biquad", parameters: { type: "Highshelf", freq: 10000.0, gain: 2.5, q: 0.71 } },
      pre_gain: { type: "Gain", parameters: { gain: -5.0, inverted: false } }
    },
    pipeline: [
      { type: "Filter", channels: [0], names: ["pre_gain", "sub_bass", "mid_dip", "treble_air"] },
      { type: "Filter", channels: [1], names: ["pre_gain", "sub_bass", "mid_dip", "treble_air"] }
    ]
  },
  vocal_clarity: {
    title: "Vocal Clarity",
    devices: { samplerate: 48000, chunksize: 128, capture: { type: "WebAudio", channels: 2 }, playback: { type: "WebAudio", channels: 2 } },
    filters: {
      high_pass: { type: "Biquad", parameters: { type: "Highpass", freq: 100.0, q: 0.707 } },
      presence: { type: "Biquad", parameters: { type: "Peaking", freq: 3200.0, gain: 3.5, q: 1.2 } },
      pre_gain: { type: "Gain", parameters: { gain: -3.5, inverted: false } }
    },
    pipeline: [
      { type: "Filter", channels: [0], names: ["pre_gain", "high_pass", "presence"] },
      { type: "Filter", channels: [1], names: ["pre_gain", "high_pass", "presence"] }
    ]
  },
  limiter_safe: {
    title: "Late Night Safety Limiter",
    devices: { samplerate: 48000, chunksize: 128, capture: { type: "WebAudio", channels: 2 }, playback: { type: "WebAudio", channels: 2 } },
    filters: {
      limiter: { type: "Limiter", parameters: { limit: -1.0, attack: 5.0, release: 100.0 } }
    },
    pipeline: [
      { type: "Filter", channels: [0], names: ["limiter"] },
      { type: "Filter", channels: [1], names: ["limiter"] }
    ]
  }
};

let currentTabId = null;
let isCaptured = false;
let isMuted = false;
let pollInterval = null;
let currentViewMode = 'both'; // 'capture' | 'playback' | 'both'
let idlePhase = 0;

// DOM Elements
const statusDot = document.getElementById('statusDot');
const tabTitle = document.getElementById('tabTitle');
const btnToggleCapture = document.getElementById('btnToggleCapture');
const btnToggleText = document.getElementById('btnToggleText');
const volumeSlider = document.getElementById('volumeSlider');
const volumeValue = document.getElementById('volumeValue');
const btnMute = document.getElementById('btnMute');
const muteIcon = document.getElementById('muteIcon');
const presetSelect = document.getElementById('presetSelect');
const configJsonText = document.getElementById('configJsonText');
const btnApplyConfig = document.getElementById('btnApplyConfig');
const btnFormatJson = document.getElementById('btnFormatJson');
const configStatusBadge = document.getElementById('configStatusBadge');

// Mode Buttons
const modeTabs = document.querySelectorAll('.mode-tab');
const spectrumTitle = document.getElementById('spectrumTitle');
const legendIn = document.getElementById('legendIn');
const legendOut = document.getElementById('legendOut');

// Meter Groups
const inputMeterGroup = document.getElementById('inputMeterGroup');
const outputMeterGroup = document.getElementById('outputMeterGroup');
const meterDivider = document.getElementById('meterDivider');

// Input Meters
const meterInFillL = document.getElementById('meterInFillL');
const meterInFillR = document.getElementById('meterInFillR');
const valInL = document.getElementById('valInL');
const valInR = document.getElementById('valInR');
const clipIndicatorIn = document.getElementById('clipIndicatorIn');

// Output Meters
const meterOutFillL = document.getElementById('meterOutFillL');
const meterOutFillR = document.getElementById('meterOutFillR');
const valOutL = document.getElementById('valOutL');
const valOutR = document.getElementById('valOutR');
const clipIndicatorOut = document.getElementById('clipIndicatorOut');

// Spectrum Canvas
const spectrumCanvas = document.getElementById('spectrumCanvas');
const canvasCtx = spectrumCanvas ? spectrumCanvas.getContext('2d') : null;

// Telemetry State
let smoothedInSpec = null;
let smoothedOutSpec = null;
let animFrameId = null;

document.addEventListener('DOMContentLoaded', async () => {
  initSpectrumCanvas();
  startSpectrumAnimation();

  // Set default initial JSON
  configJsonText.value = JSON.stringify(PRESETS.flat, null, 2);

  // Restore stored user settings and configuration
  if (chrome.storage && chrome.storage.local) {
    chrome.storage.local.get(['volumeDb', 'isMuted', 'activePreset', 'lastConfigJson', 'viewMode'], (res) => {
      if (res) {
        if (typeof res.volumeDb === 'number') {
          volumeSlider.value = res.volumeDb;
          volumeValue.textContent = (res.volumeDb > 0 ? '+' : '') + res.volumeDb.toFixed(1) + ' dB';
        }
        if (typeof res.isMuted === 'boolean') {
          isMuted = res.isMuted;
          btnMute.classList.toggle('muted', isMuted);
          muteIcon.textContent = isMuted ? '🔇' : '🔊';
        }
        if (res.activePreset && presetSelect.querySelector(`option[value="${res.activePreset}"]`)) {
          presetSelect.value = res.activePreset;
        }
        if (res.lastConfigJson) {
          configJsonText.value = res.lastConfigJson;
        }
        if (res.viewMode) {
          setViewMode(res.viewMode);
        }
      }
    });
  }

  // 1. Discover active tab
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  if (tab) {
    currentTabId = tab.id;
    tabTitle.textContent = tab.title || 'Browser Tab';

    // Query status from background
    chrome.runtime.sendMessage(
      { target: 'background', type: 'GET_STATUS', tabId: currentTabId },
      (response) => {
        if (response && response.isCaptured) {
          setCaptureState(true);
        } else {
          setCaptureState(false);
        }
      }
    );
  }

  // 2. Set up event listeners
  btnToggleCapture.addEventListener('click', onToggleCapture);
  volumeSlider.addEventListener('input', onVolumeChange);
  btnMute.addEventListener('click', onMuteToggle);
  presetSelect.addEventListener('change', onPresetChange);
  btnApplyConfig.addEventListener('click', onApplyConfig);
  btnFormatJson.addEventListener('click', onFormatJson);

  modeTabs.forEach((tabBtn) => {
    tabBtn.addEventListener('click', () => {
      const mode = tabBtn.getAttribute('data-mode');
      setViewMode(mode);
    });
  });

  window.addEventListener('resize', initSpectrumCanvas);
});

function setViewMode(mode) {
  currentViewMode = mode;
  modeTabs.forEach((btn) => {
    btn.classList.toggle('active', btn.getAttribute('data-mode') === mode);
  });

  if (mode === 'capture') {
    if (inputMeterGroup) inputMeterGroup.style.display = 'flex';
    if (outputMeterGroup) outputMeterGroup.style.display = 'none';
    if (meterDivider) meterDivider.style.display = 'none';
    if (spectrumTitle) spectrumTitle.textContent = 'Capture Spectrum (In)';
    if (legendIn) legendIn.style.display = 'flex';
    if (legendOut) legendOut.style.display = 'none';
  } else if (mode === 'playback') {
    if (inputMeterGroup) inputMeterGroup.style.display = 'none';
    if (outputMeterGroup) outputMeterGroup.style.display = 'flex';
    if (meterDivider) meterDivider.style.display = 'none';
    if (spectrumTitle) spectrumTitle.textContent = 'Playback Spectrum (Out)';
    if (legendIn) legendIn.style.display = 'none';
    if (legendOut) legendOut.style.display = 'flex';
  } else {
    // both
    if (inputMeterGroup) inputMeterGroup.style.display = 'flex';
    if (outputMeterGroup) outputMeterGroup.style.display = 'flex';
    if (meterDivider) meterDivider.style.display = 'block';
    if (spectrumTitle) spectrumTitle.textContent = 'Spectrum Analyzer';
    if (legendIn) legendIn.style.display = 'flex';
    if (legendOut) legendOut.style.display = 'flex';
  }

  if (chrome.storage && chrome.storage.local) {
    chrome.storage.local.set({ viewMode: mode });
  }
}

function initSpectrumCanvas() {
  if (!spectrumCanvas) return;
  const dpr = window.devicePixelRatio || 1;
  const parent = spectrumCanvas.parentElement;
  const w = (parent ? parent.clientWidth : 0) || 316;
  const h = (parent ? parent.clientHeight : 0) || 90;

  spectrumCanvas.width = Math.floor(w * dpr);
  spectrumCanvas.height = Math.floor(h * dpr);
  spectrumCanvas.style.width = w + 'px';
  spectrumCanvas.style.height = h + 'px';

  if (canvasCtx) {
    canvasCtx.setTransform(1, 0, 0, 1, 0, 0);
    canvasCtx.scale(dpr, dpr);
  }
}

function setCaptureState(active) {
  isCaptured = active;
  if (active) {
    statusDot.classList.add('active');
    btnToggleCapture.classList.add('active');
    btnToggleText.textContent = 'DSP Active (Click to Disable)';
    startTelemetryPolling();

    // Sync active volume & config upon starting capture
    const db = parseFloat(volumeSlider.value);
    chrome.runtime.sendMessage({
      target: 'offscreen',
      type: 'SET_VOLUME',
      tabId: currentTabId,
      fader: 0,
      db: db
    });
    chrome.runtime.sendMessage({
      target: 'offscreen',
      type: 'SET_MUTE',
      tabId: currentTabId,
      fader: 0,
      mute: isMuted
    });
    try {
      const cfg = prepareConfig(configJsonText.value);
      applyPreparedConfig(cfg);
    } catch (e) {
      // ignore
    }
  } else {
    statusDot.classList.remove('active');
    btnToggleCapture.classList.remove('active');
    btnToggleText.textContent = 'Enable DSP for this Tab';
    stopTelemetryPolling();
    resetMeters();
  }
}

async function onToggleCapture() {
  if (!currentTabId) return;

  btnToggleCapture.disabled = true;
  chrome.runtime.sendMessage(
    { target: 'background', type: 'TOGGLE_CAPTURE', tabId: currentTabId },
    (response) => {
      btnToggleCapture.disabled = false;
      if (response && response.success) {
        setCaptureState(!isCaptured);
      } else {
        alert(response?.error || 'Failed to toggle tab audio capture');
      }
    }
  );
}

function onVolumeChange() {
  const db = parseFloat(volumeSlider.value);
  volumeValue.textContent = (db > 0 ? '+' : '') + db.toFixed(1) + ' dB';

  if (chrome.storage && chrome.storage.local) {
    chrome.storage.local.set({ volumeDb: db });
  }

  if (currentTabId && isCaptured) {
    chrome.runtime.sendMessage({
      target: 'offscreen',
      type: 'SET_VOLUME',
      tabId: currentTabId,
      fader: 0,
      db: db
    });
  }
}

function onMuteToggle() {
  isMuted = !isMuted;
  btnMute.classList.toggle('muted', isMuted);
  muteIcon.textContent = isMuted ? '🔇' : '🔊';

  if (chrome.storage && chrome.storage.local) {
    chrome.storage.local.set({ isMuted: isMuted });
  }

  if (currentTabId && isCaptured) {
    chrome.runtime.sendMessage({
      target: 'offscreen',
      type: 'SET_MUTE',
      tabId: currentTabId,
      fader: 0,
      mute: isMuted
    });
  }
}

function onPresetChange() {
  const key = presetSelect.value;
  if (key === 'custom') return;

  const config = PRESETS[key];
  if (!config) return;

  const formattedJson = JSON.stringify(config, null, 2);
  configJsonText.value = formattedJson;

  if (chrome.storage && chrome.storage.local) {
    chrome.storage.local.set({
      activePreset: key,
      lastConfigJson: formattedJson
    });
  }

  applyPreparedConfig(config);
}

function onFormatJson() {
  try {
    const parsed = JSON.parse(configJsonText.value);
    const formatted = JSON.stringify(parsed, null, 2);
    configJsonText.value = formatted;
    if (chrome.storage && chrome.storage.local) {
      chrome.storage.local.set({ lastConfigJson: formatted });
    }
    showConfigStatus('Formatted', 'success');
  } catch (err) {
    showConfigStatus('JSON Error: ' + err.message, 'error');
  }
}

function prepareConfig(rawText) {
  let parsed;
  try {
    parsed = JSON.parse(rawText);
  } catch (err) {
    throw new Error('Invalid JSON: ' + err.message);
  }

  // Populate or normalize devices block for WebAudio environment
  if (!parsed.devices) {
    parsed.devices = {
      samplerate: 48000,
      chunksize: 128,
      capture: { type: "WebAudio", channels: 2 },
      playback: { type: "WebAudio", channels: 2 }
    };
  } else {
    if (!parsed.devices.samplerate) parsed.devices.samplerate = 48000;
    if (!parsed.devices.chunksize) parsed.devices.chunksize = 128;
    if (!parsed.devices.capture) parsed.devices.capture = { type: "WebAudio", channels: 2 };
    if (!parsed.devices.playback) parsed.devices.playback = { type: "WebAudio", channels: 2 };
  }

  // Normalize pipeline step 'channel' to 'channels' array if user wrote singular
  if (Array.isArray(parsed.pipeline)) {
    parsed.pipeline.forEach((step) => {
      if (step && typeof step.channel === 'number' && !step.channels) {
        step.channels = [step.channel];
        delete step.channel;
      }
    });
  }

  return parsed;
}

function onApplyConfig() {
  try {
    const config = prepareConfig(configJsonText.value);
    applyPreparedConfig(config);
    if (chrome.storage && chrome.storage.local) {
      chrome.storage.local.set({ lastConfigJson: configJsonText.value });
    }
    showConfigStatus('Applied', 'success');
  } catch (err) {
    showConfigStatus(err.message, 'error');
  }
}

function applyPreparedConfig(config) {
  if (currentTabId && isCaptured) {
    chrome.runtime.sendMessage({
      target: 'offscreen',
      type: 'SET_CONFIG',
      tabId: currentTabId,
      configJson: config
    });
  }
}

function showConfigStatus(text, type) {
  if (!configStatusBadge) return;
  configStatusBadge.textContent = text;
  configStatusBadge.className = 'config-status-badge ' + (type || '');
  setTimeout(() => {
    configStatusBadge.textContent = 'Ready';
    configStatusBadge.className = 'config-status-badge';
  }, 3500);
}


let telemetryPort = null;

function connectTelemetryPort() {
  if (telemetryPort || !currentTabId) return;
  try {
    telemetryPort = chrome.runtime.connect({ name: 'cdsp-telemetry' });
    telemetryPort.postMessage({ tabId: currentTabId });
    telemetryPort.onDisconnect.addListener(() => {
      telemetryPort = null;
    });
  } catch (e) {
    telemetryPort = null;
  }
}

function disconnectTelemetryPort() {
  if (telemetryPort) {
    try {
      telemetryPort.disconnect();
    } catch (e) {}
    telemetryPort = null;
  }
}

function startTelemetryPolling() {
  if (pollInterval) clearInterval(pollInterval);
  connectTelemetryPort();
  pollInterval = setInterval(updateTelemetry, 40);
}

function stopTelemetryPolling() {
  if (pollInterval) {
    clearInterval(pollInterval);
    pollInterval = null;
  }
  disconnectTelemetryPort();
}

function formatDb(db) {
  if (db <= -80.0 || !Number.isFinite(db)) return '-∞';
  return (db > 0 ? '+' : '') + db.toFixed(1);
}

function updateTelemetry() {
  if (!currentTabId || !isCaptured) return;

  chrome.runtime.sendMessage(
    { target: 'offscreen', type: 'GET_TELEMETRY', tabId: currentTabId },
    (res) => {
      if (res && res.telemetry) {
        const inPeak = res.telemetry.inPeak || [-120, -120];
        const outPeak = res.telemetry.outPeak || [-120, -120];

        // Input Meter
        const normInL = Number.isFinite(inPeak[0]) ? ((inPeak[0] + 60) / 60) * 100 : 0;
        const normInR = Number.isFinite(inPeak[1]) ? ((inPeak[1] + 60) / 60) * 100 : 0;
        const pctInL = Math.max(0, Math.min(100, normInL));
        const pctInR = Math.max(0, Math.min(100, normInR));
        if (meterInFillL) meterInFillL.style.width = pctInL.toFixed(1) + '%';
        if (meterInFillR) meterInFillR.style.width = pctInR.toFixed(1) + '%';
        if (valInL) valInL.textContent = formatDb(inPeak[0]);
        if (valInR) valInR.textContent = formatDb(inPeak[1]);

        if (clipIndicatorIn) {
          const isClipping = (Number.isFinite(inPeak[0]) && inPeak[0] > -0.1) ||
                             (Number.isFinite(inPeak[1]) && inPeak[1] > -0.1);
          if (isClipping) {
            clipIndicatorIn.textContent = 'CLIP';
            clipIndicatorIn.classList.add('clipped');
          } else {
            clipIndicatorIn.textContent = 'CLEAN';
            clipIndicatorIn.classList.remove('clipped');
          }
        }

        // Output Meter
        const normOutL = Number.isFinite(outPeak[0]) ? ((outPeak[0] + 60) / 60) * 100 : 0;
        const normOutR = Number.isFinite(outPeak[1]) ? ((outPeak[1] + 60) / 60) * 100 : 0;
        const pctOutL = Math.max(0, Math.min(100, normOutL));
        const pctOutR = Math.max(0, Math.min(100, normOutR));
        if (meterOutFillL) meterOutFillL.style.width = pctOutL.toFixed(1) + '%';
        if (meterOutFillR) meterOutFillR.style.width = pctOutR.toFixed(1) + '%';
        if (valOutL) valOutL.textContent = formatDb(outPeak[0]);
        if (valOutR) valOutR.textContent = formatDb(outPeak[1]);

        if (clipIndicatorOut) {
          const isClipping = (Number.isFinite(outPeak[0]) && outPeak[0] > -0.1) ||
                             (Number.isFinite(outPeak[1]) && outPeak[1] > -0.1);
          if (isClipping) {
            clipIndicatorOut.textContent = 'CLIP';
            clipIndicatorOut.classList.add('clipped');
          } else {
            clipIndicatorOut.textContent = 'CLEAN';
            clipIndicatorOut.classList.remove('clipped');
          }
        }

        // Spectrum Update
        const rawInSpec = res.telemetry.inSpectrum;
        const rawOutSpec = res.telemetry.outSpectrum;

        if (rawInSpec && rawInSpec.length > 0) {
          if (!smoothedInSpec || smoothedInSpec.length !== rawInSpec.length) {
            smoothedInSpec = new Float32Array(rawInSpec.length);
            for (let i = 0; i < rawInSpec.length; i++) {
              const v = rawInSpec[i];
              smoothedInSpec[i] = (v !== null && Number.isFinite(v)) ? v : -120.0;
            }
          } else {
            for (let i = 0; i < rawInSpec.length; i++) {
              const v = rawInSpec[i];
              const target = (v !== null && Number.isFinite(v)) ? v : -120.0;
              smoothedInSpec[i] = smoothedInSpec[i] * 0.35 + target * 0.65;
            }
          }
        }

        if (rawOutSpec && rawOutSpec.length > 0) {
          if (!smoothedOutSpec || smoothedOutSpec.length !== rawOutSpec.length) {
            smoothedOutSpec = new Float32Array(rawOutSpec.length);
            for (let i = 0; i < rawOutSpec.length; i++) {
              const v = rawOutSpec[i];
              smoothedOutSpec[i] = (v !== null && Number.isFinite(v)) ? v : -120.0;
            }
          } else {
            for (let i = 0; i < rawOutSpec.length; i++) {
              const v = rawOutSpec[i];
              const target = (v !== null && Number.isFinite(v)) ? v : -120.0;
              smoothedOutSpec[i] = smoothedOutSpec[i] * 0.35 + target * 0.65;
            }
          }
        }
      }
    }
  );
}

function startSpectrumAnimation() {
  if (animFrameId) cancelAnimationFrame(animFrameId);
  function loop() {
    drawSpectrum();
    animFrameId = requestAnimationFrame(loop);
  }
  animFrameId = requestAnimationFrame(loop);
}

function drawSpectrum() {
  if (!canvasCtx || !spectrumCanvas) return;

  const dpr = window.devicePixelRatio || 1;
  const parent = spectrumCanvas.parentElement;
  const w = (parent && parent.clientWidth > 0 ? parent.clientWidth : spectrumCanvas.clientWidth) || 316;
  const h = (parent && parent.clientHeight > 0 ? parent.clientHeight : spectrumCanvas.clientHeight) || 90;

  const targetBufferW = Math.floor(w * dpr);
  const targetBufferH = Math.floor(h * dpr);
  if (spectrumCanvas.width !== targetBufferW || spectrumCanvas.height !== targetBufferH) {
    spectrumCanvas.width = targetBufferW;
    spectrumCanvas.height = targetBufferH;
    spectrumCanvas.style.width = w + 'px';
    spectrumCanvas.style.height = h + 'px';
  }

  canvasCtx.setTransform(dpr, 0, 0, dpr, 0, 0);
  canvasCtx.clearRect(0, 0, w, h);

  // Background grid: range from +6 dBFS down to -70 dBFS (total 76 dB range)
  const topDb = 6.0;
  const bottomDb = -70.0;
  const dbSpan = topDb - bottomDb; // 76.0 dB

  canvasCtx.strokeStyle = 'rgba(51, 65, 85, 0.4)';
  canvasCtx.lineWidth = 1;

  const dbLines = [
    { db: 0, label: '0dB' },
    { db: -20, label: '-20' },
    { db: -40, label: '-40' },
    { db: -60, label: '-60' }
  ];
  canvasCtx.font = '8px monospace';
  canvasCtx.fillStyle = 'rgba(100, 116, 139, 0.6)';
  dbLines.forEach(({ db, label }) => {
    const y = ((topDb - db) / dbSpan) * h;
    canvasCtx.beginPath();
    canvasCtx.moveTo(0, y);
    canvasCtx.lineTo(w, y);
    canvasCtx.stroke();
    canvasCtx.fillText(label, 3, Math.max(8, y - 2));
  });

  const freqMarks = [100, 1000, 10000];
  freqMarks.forEach((freq) => {
    const normX = Math.log10(freq / 20.0) / Math.log10(20000.0 / 20.0);
    const x = normX * w;
    canvasCtx.beginPath();
    canvasCtx.moveTo(x, 0);
    canvasCtx.lineTo(x, h);
    canvasCtx.stroke();
  });

  if (!isCaptured) {
    const n = 48;
    const idleBins = new Float32Array(n).fill(bottomDb);
    renderSpectrumCurve(idleBins, w, h, 'rgba(100, 116, 139, 0.4)', 'rgba(100, 116, 139, 0.04)', 1.5);
    return;
  }

  if (currentViewMode === 'both' || currentViewMode === 'capture') {
    if (smoothedInSpec && smoothedInSpec.length > 1) {
      const isSolo = (currentViewMode === 'capture');
      renderSpectrumCurve(
        smoothedInSpec,
        w,
        h,
        'rgba(6, 182, 212, 0.95)',
        isSolo ? 'rgba(6, 182, 212, 0.35)' : 'rgba(6, 182, 212, 0.12)',
        isSolo ? 2.5 : 1.8
      );
    }
  }

  if (currentViewMode === 'both' || currentViewMode === 'playback') {
    if (smoothedOutSpec && smoothedOutSpec.length > 1) {
      const isSolo = (currentViewMode === 'playback');
      renderSpectrumCurve(
        smoothedOutSpec,
        w,
        h,
        'rgba(16, 185, 129, 0.98)',
        isSolo ? 'rgba(16, 185, 129, 0.45)' : 'rgba(16, 185, 129, 0.25)',
        2.5
      );
    }
  }
}

function renderSpectrumCurve(bins, w, h, strokeColor, fillColor, lineWidth) {
  const n = bins.length;
  if (n < 2) return;

  const topDb = 6.0;
  const bottomDb = -70.0;
  const dbSpan = topDb - bottomDb; // 76.0 dB

  const points = [];
  for (let i = 0; i < n; i++) {
    const x = (i / (n - 1)) * w;
    const rawDb = Number.isFinite(bins[i]) ? bins[i] : bottomDb;
    const db = Math.max(bottomDb, Math.min(topDb, rawDb));
    const y = ((topDb - db) / dbSpan) * h;
    points.push({ x, y });
  }

  canvasCtx.beginPath();
  canvasCtx.moveTo(0, h);
  canvasCtx.lineTo(points[0].x, points[0].y);
  for (let i = 0; i < points.length - 1; i++) {
    const xc = (points[i].x + points[i + 1].x) / 2;
    const yc = (points[i].y + points[i + 1].y) / 2;
    canvasCtx.quadraticCurveTo(points[i].x, points[i].y, xc, yc);
  }
  canvasCtx.lineTo(points[points.length - 1].x, points[points.length - 1].y);
  canvasCtx.lineTo(w, h);
  canvasCtx.closePath();

  const grad = canvasCtx.createLinearGradient(0, 0, 0, h);
  grad.addColorStop(0, fillColor);
  grad.addColorStop(1, 'rgba(0, 0, 0, 0)');
  canvasCtx.fillStyle = grad;
  canvasCtx.fill();

  canvasCtx.beginPath();
  canvasCtx.moveTo(points[0].x, points[0].y);
  for (let i = 0; i < points.length - 1; i++) {
    const xc = (points[i].x + points[i + 1].x) / 2;
    const yc = (points[i].y + points[i + 1].y) / 2;
    canvasCtx.quadraticCurveTo(points[i].x, points[i].y, xc, yc);
  }
  canvasCtx.lineTo(points[points.length - 1].x, points[points.length - 1].y);
  canvasCtx.strokeStyle = strokeColor;
  canvasCtx.lineWidth = lineWidth;
  canvasCtx.stroke();
}

function resetMeters() {
  if (meterInFillL) meterInFillL.style.width = '0%';
  if (meterInFillR) meterInFillR.style.width = '0%';
  if (valInL) valInL.textContent = '-∞';
  if (valInR) valInR.textContent = '-∞';
  if (clipIndicatorIn) {
    clipIndicatorIn.textContent = 'CLEAN';
    clipIndicatorIn.classList.remove('clipped');
  }

  if (meterOutFillL) meterOutFillL.style.width = '0%';
  if (meterOutFillR) meterOutFillR.style.width = '0%';
  if (valOutL) valOutL.textContent = '-∞';
  if (valOutR) valOutR.textContent = '-∞';
  if (clipIndicatorOut) {
    clipIndicatorOut.textContent = 'CLEAN';
    clipIndicatorOut.classList.remove('clipped');
  }

  smoothedInSpec = null;
  smoothedOutSpec = null;
}
