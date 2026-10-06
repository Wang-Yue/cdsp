/**
 * @file studio-bootstrap.js
 * @brief External bootstrap script for Qt WebAssembly Studio to comply with Manifest V3 CSP.
 */

async function initStudio() {
  const spinner = document.querySelector('#qtspinner');
  const screen = document.querySelector('#screen');
  const status = document.querySelector('#qtstatus');

  const showUi = (ui) => {
    [spinner, screen].forEach((element) => {
      if (element) element.style.display = 'none';
    });
    if (screen === ui) {
      screen.style.display = 'block';
    } else if (ui) {
      ui.style.display = 'flex';
    }
  };

  try {
    showUi(spinner);
    if (status) status.innerHTML = 'Loading WebAssembly module...';

    const entryFn = window.cdsp_studio || window.cdsp_studio_entry || window.createCdspStudioModule || window.Module;
    if (typeof entryFn !== 'function') {
      throw new Error(`Qt WebAssembly entry function not found on window (window.cdsp_studio = ${typeof window.cdsp_studio})`);
    }

    // The multithreaded Qt build needs SharedArrayBuffer to hand its shared wasm memory to the
    // pthread pool workers. Without cross-origin isolation (COOP/COEP) that handoff fails and
    // Emscripten waits forever on the pool, leaving the spinner stuck with no error.
    if (!self.crossOriginIsolated || typeof SharedArrayBuffer === 'undefined') {
      throw new Error('Page is not cross-origin isolated (SharedArrayBuffer unavailable). ' +
                      'Ensure manifest.json sets cross_origin_opener_policy=same-origin and ' +
                      'cross_origin_embedder_policy=require-corp, then reload the extension.');
    }

    await qtLoad({
      qt: {
        onLoaded: () => {
          showUi(screen);
        },
        onExit: (exitData) => {
          if (status) {
            status.innerHTML = 'Application exit';
            if (exitData && exitData.code !== undefined) {
              status.innerHTML += ` with code ${exitData.code}`;
            }
            if (exitData && exitData.text !== undefined) {
              status.innerHTML += ` (${exitData.text})`;
            }
          }
          showUi(spinner);
        },
        entryFunction: entryFn,
        containerElements: [screen]
      },
      // Qt 6.7's wasm QMediaDevices has a leftover uncategorized qWarning() that prints
      // "m_audioInputs count N" on every device enumeration. Emscripten sends stderr to
      // console.error, so Chrome's extension page reports it as an error. Drop that one line.
      printErr: (text) => {
        if (!text.startsWith('m_audioInputs count')) {
          console.error(text);
        }
      }
    });
  } catch (e) {
    if (status) {
      status.innerHTML = 'Failed to launch Qt Studio: ' + (e ? e.message : 'Unknown error');
      status.style.color = '#f43f5e';
    }
    console.error('Qt WebAssembly Studio initialization failed:', e);
  }
}

// Tab-capture source for the engine's WebAudio capture backend.
//
// The engine itself runs inside the Qt WebAssembly module, exactly as on desktop: its capture,
// processing and playback threads are pthreads, and its "sound card" is an AudioContext whose
// Wasm AudioWorklet node calls the WebAudio backend on the real-time audio thread (see
// webaudio_device_start(), run by dsp_engine_create()). This page only connects the captured tab's MediaStream to that
// node while the engine has its WebAudio capture backend open.
window.cdspBridge = {
  captureWanted: false,
  stream: null,
  source: null,
  tabId: null,
  starting: null,
  stopTimer: null,

  init: function() {
    window.addEventListener('cdsp-audio-ready', () => this.onAudioReady());
    // Autoplay policy: the AudioContext stays suspended until the first user gesture.
    const resume = () => this.resumeAudio();
    document.addEventListener('pointerdown', resume, true);
    document.addEventListener('keydown', resume, true);
    window.addEventListener('beforeunload', (event) => this.onBeforeUnload(event));
    // Restored (from the shelf/taskbar or the CDSP icon): the background notice is stale.
    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'visible' && typeof chrome !== 'undefined' && chrome.runtime) {
        this.sendToBackground({ type: 'BACKGROUND_NOTICE', show: false }).catch(() => {});
      }
    });
  },

  /**
   * Closing the window while the DSP runs. A page cannot show its own dialog here, only Chrome's
   * fixed "Leave site?" prompt:
   *   - Leave:  the window closes and the DSP stops with it.
   *   - Cancel: the page stays, and the timer below (which only fires if the page survived)
   *             minimizes the window, so the DSP keeps running in the background.
   */
  onBeforeUnload: function(event) {
    if (!this.captureWanted && !this.stream) return; // DSP not running: just close.
    event.preventDefault();
    event.returnValue = '';
    setTimeout(() => this.minimizeWindow(), 0);
  },

  minimizeWindow: async function() {
    if (typeof chrome === 'undefined' || !chrome.windows) return;
    try {
      const win = await chrome.windows.getCurrent();
      await chrome.windows.update(win.id, { state: 'minimized' });
      this.sendToBackground({ type: 'BACKGROUND_NOTICE', show: true }).catch(() => {});
    } catch (e) {
      console.warn('[CDSP] Minimizing the Studio window failed:', e.message || e);
    }
  },

  /**
   * One-time tip, shown the first time the DSP runs: Chrome's close prompt has fixed wording
   * ("Leave site? Changes you made may not be saved"), so explain what its buttons do here.
   */
  showCloseTip: async function() {
    if (typeof chrome === 'undefined' || !chrome.storage) return;
    const { closeTipShown } = await chrome.storage.local.get('closeTipShown');
    if (closeTipShown || document.getElementById('cdsp-close-tip')) return;
    const tip = document.createElement('div');
    tip.id = 'cdsp-close-tip';
    tip.style.cssText =
      'position:fixed;left:50%;bottom:24px;transform:translateX(-50%);z-index:10000;max-width:560px;' +
      'padding:12px 16px;border-radius:8px;background:#1e293b;color:#f8fafc;font-size:13px;' +
      'line-height:1.45;box-shadow:0 8px 24px rgba(0,0,0,.45);display:flex;gap:12px;align-items:center;';
    const text = document.createElement('div');
    text.innerHTML =
      '<b>Keep the DSP running after closing Studio</b><br>' +
      'When you close this window, Chrome asks <i>“Leave site?”</i>. Choose <b>Cancel</b> to ' +
      'minimize Studio and keep processing in the background, or <b>Leave</b> to stop the DSP.';
    const ok = document.createElement('button');
    ok.textContent = 'Got it';
    ok.style.cssText =
      'flex:none;padding:6px 14px;border:0;border-radius:6px;background:#06b6d4;color:#0f172a;' +
      'font-weight:600;cursor:pointer;';
    ok.addEventListener('click', () => {
      tip.remove();
      chrome.storage.local.set({ closeTipShown: true }).catch(() => {});
    });
    tip.append(text, ok);
    document.body.appendChild(tip);
  },

  audio: function() {
    return globalThis.cdspAudio || null;
  },

  /** Resumes the capture and playback contexts (each has its own clock). */
  resumeAudio: function() {
    [this.audio(), globalThis.cdspPlaybackAudio].forEach((audio) => {
      if (audio && audio.context.state === 'suspended') {
        audio.context.resume().catch(() => {});
      }
    });
  },

  /** A device context (re)started: at startup, or recreated in a new format. */
  onAudioReady: function() {
    this.resumeAudio();
    if (this.stream) {
      this.connectSource();
    } else if (this.captureWanted) {
      this.ensureCapture();
    }
  },

  /** Feeds the captured stream into the current device node. */
  connectSource: function() {
    const audio = this.audio();
    if (this.source) this.source.disconnect();
    this.source = null;
    if (!audio || !this.stream) return;
    this.source = audio.context.createMediaStreamSource(this.stream);
    this.source.connect(audio.node);
  },

  /**
   * Called from the engine (webaudio_device.c) when its WebAudio capture backend attaches to or
   * detaches from the device.
   */
  onCaptureAttached: function(attached) {
    this.captureWanted = attached;
    clearTimeout(this.stopTimer);
    if (attached) {
      this.ensureCapture();
    } else {
      // A config change closes and reopens the backend; keep the tab captured across that.
      this.stopTimer = setTimeout(() => {
        if (!this.captureWanted) this.stopCapture();
      }, 2000);
    }
  },

  sendToBackground: function(message) {
    return new Promise((resolve, reject) => {
      chrome.runtime.sendMessage({ target: 'background', ...message }, (res) => {
        if (chrome.runtime.lastError) {
          reject(new Error(chrome.runtime.lastError.message));
        } else {
          resolve(res);
        }
      });
    });
  },

  ensureCapture: function() {
    if (this.stream || this.starting || !this.audio()) return;
    if (typeof chrome === 'undefined' || !chrome.tabCapture) return;
    this.starting = this.startCapture()
      .catch((e) => console.error('[CDSP] Tab capture failed:', e.message || e))
      .finally(() => { this.starting = null; });
  },

  /**
   * Captures the target tab in this page. chrome.tabCapture.getMediaStreamId() binds the stream
   * ID to the caller's renderer process, so it is minted here, where getUserMedia() consumes it.
   */
  startCapture: async function() {
    const target = await this.sendToBackground({ type: 'RESOLVE_CAPTURE_TARGET' });
    if (!target || !target.tabId) {
      throw new Error('No capturable tab found');
    }
    const streamId = await chrome.tabCapture.getMediaStreamId({ targetTabId: target.tabId });
    const stream = await navigator.mediaDevices.getUserMedia({
      audio: { mandatory: { chromeMediaSource: 'tab', chromeMediaSourceId: streamId } },
      video: false
    });
    if (!this.captureWanted) {
      stream.getTracks().forEach((t) => t.stop());
      return;
    }
    this.stream = stream;
    this.tabId = target.tabId;
    this.connectSource();
    stream.getAudioTracks().forEach((t) => t.addEventListener('ended', () => this.stopCapture()));
    this.resumeAudio();
    this.sendToBackground({ type: 'CAPTURE_STATE', tabId: target.tabId, active: true }).catch(() => {});
    console.info('[CDSP] Capturing tab', target.tabId);
    this.showCloseTip().catch(() => {});
    // Report the captured stream's rate and channel count to the WebAudio device, which runs its
    // capture context at that rate and reports a format change if either differs from the running
    // one. They can change mid-stream, so keep checking.
    this.reportInputFormat();
    clearInterval(this.channelPoll);
    this.channelPoll = setInterval(() => this.reportInputFormat(), 2000);
  },

  reportInputFormat: function() {
    const track = this.stream && this.stream.getAudioTracks()[0];
    const device = globalThis.cdspAudioDevice;
    if (!track || !device) return;
    const settings = track.getSettings();
    device.setInputFormat(settings.sampleRate || 0, settings.channelCount || 2);
  },

  stopCapture: function() {
    clearInterval(this.channelPoll);
    this.channelPoll = null;
    if (this.source) {
      this.source.disconnect();
      this.source = null;
    }
    if (this.stream) {
      this.stream.getTracks().forEach((t) => t.stop());
      this.stream = null;
    }
    if (this.tabId) {
      this.sendToBackground({ type: 'CAPTURE_STATE', tabId: this.tabId, active: false }).catch(() => {});
      this.tabId = null;
    }
  }
};
window.cdspBridge.init();

// Sizes the extension's Studio popup window to the current Studio mode. Called from Studio
// (MainWindow.cpp) when it switches between the main window and the mini player; both fill the
// page, so resizing the Chrome window resizes the Qt UI. The service worker remembers the
// window bounds of each mode separately (see studioMode in service_worker.js).
window.cdspStudioWindow = {
  setMode: async function(mode, miniWidth, miniHeight) {
    if (typeof chrome === 'undefined' || !chrome.windows || !chrome.storage) return;
    try {
      const win = await chrome.windows.getCurrent();
      // Only resize Studio's own popup window, never a normal browser window it was opened in.
      if (win.type !== 'popup') return;
      if (mode === 'mini' && win.state === 'normal') {
        // Snapshot the main window's bounds so leaving the mini player can restore them.
        const { left, top, width, height } = win;
        await chrome.storage.local.set({ studioBounds: { left, top, width, height } });
      }
      await chrome.storage.session.set({ studioMode: mode });
      const key = mode === 'mini' ? 'studioMiniBounds' : 'studioBounds';
      let bounds = (await chrome.storage.local.get(key))[key];
      if (!bounds && mode === 'mini') {
        // First time: fit the mini player, keeping the window's top-left corner.
        const frameW = window.outerWidth - window.innerWidth;
        const frameH = window.outerHeight - window.innerHeight;
        bounds = { left: win.left, top: win.top, width: miniWidth + frameW, height: miniHeight + frameH };
      }
      if (!bounds) return;
      if (win.state !== 'normal') await chrome.windows.update(win.id, { state: 'normal' });
      try {
        await chrome.windows.update(win.id, bounds);
      } catch (e) {
        // Remembered position may be off-screen now; keep the size only.
        await chrome.windows.update(win.id, { width: bounds.width, height: bounds.height });
      }
    } catch (e) {
      console.warn('[CDSP] Resizing the Studio window failed:', e.message || e);
    }
  }
};

if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', initStudio);
} else {
  initStudio();
}
