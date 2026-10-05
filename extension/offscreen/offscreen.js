/**
 * @file offscreen.js
 * @brief Web Audio host running inside Chrome Offscreen Document.
 *
 * Captures tab audio MediaStream, runs the CDSP AudioWorkletProcessor,
 * and streams the filtered audio out to audioContext.destination.
 */

// Map of active tab sessions: tabId -> { audioCtx, sourceNode, cdspNode, stream, telemetry }
const sessions = new Map();

/**
 * Loads persisted settings from chrome.storage.local.
 */
async function getStoredSettings() {
  return new Promise((resolve) => {
    if (chrome.storage && chrome.storage.local) {
      chrome.storage.local.get(['volumeDb', 'isMuted', 'lastConfigJson'], (res) => {
        resolve({
          volumeDb: res?.volumeDb ?? 0.0,
          isMuted: !!res?.isMuted,
          configJson: res?.lastConfigJson ? JSON.parse(res.lastConfigJson) : null
        });
      });
    } else {
      resolve({ volumeDb: 0.0, isMuted: false, configJson: null });
    }
  });
}

/**
 * Starts processing for a captured tab.
 */
async function handleStartCapture(tabId, streamId) {
  try {
    if (sessions.has(tabId)) {
      handleStopCapture(tabId);
    }

    // 1. Ingest tab audio via getUserMedia with chromeMediaSource
    const stream = await navigator.mediaDevices.getUserMedia({
      audio: {
        mandatory: {
          chromeMediaSource: 'tab',
          chromeMediaSourceId: streamId
        }
      },
      video: false
    });

    // 2. Initialize AudioContext at standard 48kHz interactive rate
    const audioCtx = new AudioContext({
      sampleRate: 48000,
      latencyHint: 'interactive'
    });

    // Resume context if suspended
    if (audioCtx.state === 'suspended') {
      await audioCtx.resume();
    }

    // 3. Attempt to fetch WASM binary and load WASM helper module if available
    let wasmBytes = null;
    try {
      const wasmRes = await fetch(chrome.runtime.getURL('wasm/cdsp_wasm.wasm'));
      if (wasmRes.ok) {
        wasmBytes = await wasmRes.arrayBuffer();
      }
    } catch (e) {
      // WASM binary optional
    }

    try {
      const wasmJsUrl = chrome.runtime.getURL('wasm/cdsp_wasm.js');
      await audioCtx.audioWorklet.addModule(wasmJsUrl);
    } catch (e) {
      // WASM JS optional
    }

    // 4. Load CDSP AudioWorkletProcessor module
    const workletUrl = chrome.runtime.getURL('worklet/cdsp-processor.js');
    await audioCtx.audioWorklet.addModule(workletUrl);

    // 5. Retrieve stored user settings for initial configuration
    const stored = await getStoredSettings();

    // 6. Instantiate Worklet Node with initial persisted parameters
    const cdspNode = new AudioWorkletNode(audioCtx, 'cdsp-processor', {
      numberOfInputs: 1,
      numberOfOutputs: 1,
      outputChannelCount: [2],
      processorOptions: {
        wasmBytes: wasmBytes,
        volumeDb: stored.volumeDb,
        muted: stored.isMuted,
        initialConfig: stored.configJson
      }
    });

    const session = {
      audioCtx,
      stream,
      cdspNode,
      telemetry: {
        inPeak: [-120, -120],
        inRms: [-120, -120],
        outPeak: [-120, -120],
        outRms: [-120, -120],
        inSpectrum: new Array(48).fill(-120),
        outSpectrum: new Array(48).fill(-120)
      }
    };

    // 7. Telemetry listener from AudioWorklet thread
    cdspNode.port.onmessage = (event) => {
      const data = event.data;
      if (data && data.type === 'TELEMETRY') {
        session.telemetry = data.telemetry;
      }
    };

    // 8. Connect audio graph: Tab -> DSP -> System Output
    const sourceNode = audioCtx.createMediaStreamSource(stream);
    session.sourceNode = sourceNode;

    sourceNode.connect(cdspNode);
    cdspNode.connect(audioCtx.destination);

    sessions.set(tabId, session);
    console.log(`[CDSP Offscreen] Tab ${tabId} captured and processing started.`);
    return { success: true };
  } catch (err) {
    console.error(`[CDSP Offscreen] Error starting capture for tab ${tabId}:`, err);
    return { success: false, error: err.message };
  }
}

/**
 * Stops processing and tears down audio nodes for a tab.
 */
function handleStopCapture(tabId) {
  const session = sessions.get(tabId);
  if (!session) {
    return { success: true };
  }

  try {
    session.sourceNode?.disconnect();
    session.cdspNode?.disconnect();
    session.stream?.getTracks().forEach((track) => track.stop());
    session.audioCtx?.close();
  } catch (e) {
    console.warn('[CDSP Offscreen] Cleanup warning:', e);
  }

  sessions.delete(tabId);
  console.log(`[CDSP Offscreen] Tab ${tabId} capture stopped.`);
  return { success: true };
}

// IPC message handler from service worker, popup, and studio GUI
chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
  if (message.target !== 'offscreen') {
    return false;
  }

  if (message.type === 'START_TAB_CAPTURE') {
    handleStartCapture(message.tabId, message.streamId).then(sendResponse);
    return true;
  }

  if (message.type === 'STOP_TAB_CAPTURE') {
    const res = handleStopCapture(message.tabId);
    sendResponse(res);
    return false;
  }

  if (message.type === 'SET_VOLUME') {
    const session = sessions.get(message.tabId);
    if (session?.cdspNode) {
      session.cdspNode.port.postMessage({
        type: 'SET_VOLUME',
        fader: message.fader ?? 0,
        db: message.db ?? 0.0,
        instant: message.instant ?? false
      });
    }
    sendResponse({ success: true });
    return false;
  }

  if (message.type === 'SET_MUTE') {
    const session = sessions.get(message.tabId);
    if (session?.cdspNode) {
      session.cdspNode.port.postMessage({
        type: 'SET_MUTE',
        fader: message.fader ?? 0,
        mute: message.mute ?? false
      });
    }
    sendResponse({ success: true });
    return false;
  }

  if (message.type === 'SET_CONFIG') {
    const session = sessions.get(message.tabId);
    if (session?.cdspNode) {
      session.cdspNode.port.postMessage({
        type: 'SET_CONFIG',
        configJson: message.configJson
      });
    }
    sendResponse({ success: true });
    return false;
  }

  if (message.type === 'SET_TELEMETRY_ACTIVE') {
    const session = sessions.get(message.tabId);
    if (session?.cdspNode) {
      session.cdspNode.port.postMessage({
        type: 'SET_TELEMETRY_ACTIVE',
        active: !!message.active
      });
    }
    sendResponse({ success: true });
    return false;
  }

  if (message.type === 'GET_TELEMETRY') {
    const session = sessions.get(message.tabId);
    sendResponse({
      active: !!session,
      telemetry: session?.telemetry || null
    });
    return false;
  }

  return false;
});

// Auto-enable telemetry when popup connects, auto-disable when popup disconnects
chrome.runtime.onConnect.addListener((port) => {
  if (port.name === 'cdsp-telemetry') {
    let boundTabId = null;
    port.onMessage.addListener((msg) => {
      if (msg && msg.tabId) {
        boundTabId = msg.tabId;
        const session = sessions.get(boundTabId);
        if (session?.cdspNode) {
          session.cdspNode.port.postMessage({
            type: 'SET_TELEMETRY_ACTIVE',
            active: true
          });
        }
      }
    });
    port.onDisconnect.addListener(() => {
      if (boundTabId) {
        const session = sessions.get(boundTabId);
        if (session?.cdspNode) {
          session.cdspNode.port.postMessage({
            type: 'SET_TELEMETRY_ACTIVE',
            active: false
          });
        }
      }
    });
  }
});
