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

// Global DSP bridge between Qt WebAssembly C++ engine and Chrome Extension WebAudio host
window.cdspBridge = {
  isCaptured: false,
  activeTabId: null,
  volumeDb: 0.0,
  isMuted: false,
  telemetry: {
    inPeak: [-120.0, -120.0],
    inRms: [-120.0, -120.0],
    outPeak: [-120.0, -120.0],
    outRms: [-120.0, -120.0],
    inSpectrum: new Array(48).fill(-120.0),
    outSpectrum: new Array(48).fill(-120.0),
    inSamplesL: [],
    inSamplesR: [],
    outSamplesL: [],
    outSamplesR: []
  },

  init: function() {
    this.loadStoredSettings();
    this.pollStatus();
    this.startTelemetryStream();
  },

  loadStoredSettings: function() {
    try {
      if (typeof chrome !== 'undefined' && chrome.storage && chrome.storage.local) {
        chrome.storage.local.get(['volumeDb', 'isMuted', 'lastTargetTabId'], (res) => {
          if (res) {
            if (typeof res.volumeDb === 'number') this.volumeDb = res.volumeDb;
            if (typeof res.isMuted === 'boolean') this.isMuted = res.isMuted;
            if (res.lastTargetTabId) this.activeTabId = res.lastTargetTabId;
          }
        });
        chrome.storage.onChanged.addListener((changes, area) => {
          if (area === 'local') {
            if (changes.volumeDb && typeof changes.volumeDb.newValue === 'number') {
              this.volumeDb = changes.volumeDb.newValue;
            }
            if (changes.isMuted && typeof changes.isMuted.newValue === 'boolean') {
              this.isMuted = changes.isMuted.newValue;
            }
          }
        });
      }
    } catch (e) {}
  },

  pollStatus: function() {
    try {
      if (typeof chrome !== 'undefined' && chrome.runtime && chrome.runtime.sendMessage) {
        chrome.runtime.sendMessage({ target: 'background', type: 'GET_STATUS' }, (res) => {
          if (chrome.runtime.lastError) return;
          if (res) {
            this.isCaptured = !!res.isCaptured;
            if (res.activeTabIds && res.activeTabIds.length > 0) {
              this.activeTabId = res.activeTabIds[0];
            }
          }
        });
      }
    } catch (e) {}
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

  /**
   * Starts capture of a tab, minting the tab-capture stream ID in this page.
   *
   * In MV3 Chrome binds a stream ID from chrome.tabCapture.getMediaStreamId() to the caller's
   * renderer process. Extension pages are cross-origin isolated (COOP/COEP in manifest.json,
   * required for SharedArrayBuffer by the multithreaded Qt WASM), so they no longer share the
   * service worker's process; an ID minted there makes getUserMedia() in the offscreen document
   * fail with "AbortError: Error starting tab capture". Minting it here puts it in the isolated
   * extension process that the offscreen document also uses.
   */
  startCaptureAsync: async function(tabId) {
    const target = await this.sendToBackground({
      type: 'RESOLVE_CAPTURE_TARGET',
      tabId: tabId || this.activeTabId
    });
    if (!target || !target.tabId) {
      throw new Error('No captureable tab found');
    }
    let streamId;
    if (!target.isCaptured) {
      streamId = await chrome.tabCapture.getMediaStreamId({ targetTabId: target.tabId });
    }
    const res = await this.sendToBackground({
      type: 'START_CAPTURE',
      tabId: target.tabId,
      streamId
    });
    if (!res || !res.success) {
      throw new Error(res ? res.error : 'no response');
    }
    this.isCaptured = true;
    this.activeTabId = res.tabId || target.tabId;
    this.pollStatus();
  },

  startCapture: function(tabId) {
    // CDSPEngine::start() calls this on every config change until isCaptured flips, which only
    // happens asynchronously; don't stack up duplicate capture requests meanwhile.
    if (this.captureStartPending) return;
    if (typeof chrome === 'undefined' || !chrome.runtime || !chrome.tabCapture) return;
    this.captureStartPending = true;
    this.startCaptureAsync(tabId)
      .catch((e) => console.error('[CDSP Bridge] startCapture failed:', e.message || e))
      .finally(() => { this.captureStartPending = false; });
  },

  stopCapture: function(tabId) {
    try {
      if (typeof chrome !== 'undefined' && chrome.runtime && chrome.runtime.sendMessage) {
        chrome.runtime.sendMessage({
          target: 'background',
          type: 'STOP_CAPTURE',
          tabId: tabId || this.activeTabId
        }, (res) => {
          this.isCaptured = false;
          this.pollStatus();
        });
      }
    } catch (e) {
      console.error('[CDSP Bridge] stopCapture failed:', e);
    }
  },

  toggleCapture: function(tabId) {
    if (this.isCaptured) {
      this.stopCapture(tabId);
    } else {
      this.startCapture(tabId);
    }
  },

  startTelemetryStream: function() {
    try {
      if (typeof chrome !== 'undefined' && chrome.runtime && chrome.runtime.sendMessage) {
        chrome.runtime.sendMessage({ target: 'offscreen', type: 'SET_TELEMETRY_ACTIVE', active: true });
      }
    } catch (e) {}

    setInterval(() => {
      try {
        if (typeof chrome !== 'undefined' && chrome.runtime && chrome.runtime.sendMessage) {
          chrome.runtime.sendMessage({ target: 'offscreen', type: 'GET_TELEMETRY' }, (res) => {
            if (chrome.runtime.lastError || !res) return;
            if (res.isCaptured !== undefined) {
              this.isCaptured = !!res.isCaptured;
            } else if (res.active !== undefined) {
              this.isCaptured = !!res.active;
            }
            if (res.telemetry) {
              this.telemetry = res.telemetry;
            }
          });
        }
      } catch (e) {}
    }, 40);
  },

  setConfig: function(configJsonStr) {
    if (!configJsonStr) return;
    try {
      if (typeof chrome !== 'undefined') {
        if (chrome.storage && chrome.storage.local) {
          chrome.storage.local.set({ lastConfigJson: configJsonStr });
        }
        let parsed = null;
        try {
          parsed = JSON.parse(configJsonStr);
        } catch (e) {}
        if (chrome.runtime && chrome.runtime.sendMessage) {
          chrome.runtime.sendMessage({
            target: 'offscreen',
            type: 'SET_CONFIG',
            tabId: this.activeTabId,
            configJson: parsed || configJsonStr
          });
        }
      }
    } catch (e) {
      console.error('[CDSP Bridge] setConfig failed:', e);
    }
  },

  setFaderVolume: function(fader, db, instant) {
    this.volumeDb = db;
    try {
      if (typeof chrome !== 'undefined') {
        if (chrome.storage && chrome.storage.local && fader === 0) {
          chrome.storage.local.set({ volumeDb: db });
        }
        if (chrome.runtime && chrome.runtime.sendMessage) {
          chrome.runtime.sendMessage({
            target: 'offscreen',
            type: 'SET_VOLUME',
            tabId: this.activeTabId,
            fader: fader,
            db: db,
            instant: !!instant
          });
        }
      }
    } catch (e) {}
  },

  setFaderMute: function(fader, mute) {
    this.isMuted = !!mute;
    try {
      if (typeof chrome !== 'undefined') {
        if (chrome.storage && chrome.storage.local && fader === 0) {
          chrome.storage.local.set({ isMuted: !!mute });
        }
        if (chrome.runtime && chrome.runtime.sendMessage) {
          chrome.runtime.sendMessage({
            target: 'offscreen',
            type: 'SET_MUTE',
            tabId: this.activeTabId,
            fader: fader,
            mute: !!mute
          });
        }
      }
    } catch (e) {}
  }
};
window.cdspBridge.init();

if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', initStudio);
} else {
  initStudio();
}
