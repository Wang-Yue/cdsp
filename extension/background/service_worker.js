/**
 * @file service_worker.js
 * @brief Chrome Extension Background Service Worker (Manifest V3)
 *
 * Coordinates tab capture lifecycle, offscreen document management,
 * and IPC message dispatching between the UI and Web Audio offscreen host.
 */

const OFFSCREEN_DOCUMENT_PATH = 'offscreen/offscreen.html';

// Set of active captured tab IDs
const activeCapturedTabs = new Set();

/**
 * Ensures the offscreen document is loaded and ready.
 */
async function ensureOffscreenDocument() {
  if (await chrome.offscreen.hasDocument()) {
    return;
  }
  await chrome.offscreen.createDocument({
    url: OFFSCREEN_DOCUMENT_PATH,
    reasons: ['AUDIO_PLAYBACK'],
    justification: 'Real-time DSP filtering and playback of tab audio streams'
  });

  // Wait for offscreen document scripts to initialize and attach message listener
  for (let attempt = 0; attempt < 25; attempt++) {
    try {
      const isReady = await new Promise((resolve) => {
        chrome.runtime.sendMessage({ target: 'offscreen', type: 'PING' }, (resp) => {
          if (chrome.runtime.lastError) {
            resolve(false);
          } else {
            resolve(resp?.ready === true);
          }
        });
      });
      if (isReady) return;
    } catch (e) {}
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
}

// In-flight capture starts keyed by tab ID, so concurrent requests share one capture.
const pendingCaptureStarts = new Map();

/**
 * Starts audio capture on a target tab.
 *
 * Chrome allows only one active capture per tab; a second getMediaStreamId/getUserMedia for
 * the same tab fails with "AbortError: Error starting tab capture". Requests for a tab that is
 * already captured, or whose capture is in flight, are therefore coalesced.
 *
 * @param {number} tabId Tab to capture.
 * @param {string=} streamId Stream ID from chrome.tabCapture.getMediaStreamId(), obtained by an
 *     extension page. In MV3 Chrome binds the ID to the caller's renderer process, and because
 *     extension pages are cross-origin isolated (COOP/COEP in manifest.json, needed by the
 *     multithreaded Studio WASM) they don't share the service worker's process. An ID minted
 *     here would make getUserMedia in the offscreen document fail with "Error starting tab
 *     capture", so callers should mint it in an extension page and pass it in.
 */
function startTabCapture(tabId, streamId) {
  if (activeCapturedTabs.has(tabId)) {
    return Promise.resolve({ success: true });
  }
  if (pendingCaptureStarts.has(tabId)) {
    return pendingCaptureStarts.get(tabId);
  }
  const pending = doStartTabCapture(tabId, streamId)
    .finally(() => pendingCaptureStarts.delete(tabId));
  pendingCaptureStarts.set(tabId, pending);
  return pending;
}

async function doStartTabCapture(tabId, providedStreamId) {
  try {
    await ensureOffscreenDocument();

    // The service worker may have been restarted (losing activeCapturedTabs) while the offscreen
    // document kept capturing; it is the source of truth for live captures.
    const existing = await new Promise((resolve) => {
      chrome.runtime.sendMessage({ target: 'offscreen', type: 'IS_TAB_CAPTURED', tabId }, (resp) => {
        resolve(!chrome.runtime.lastError && resp?.captured === true);
      });
    });
    if (existing) {
      activeCapturedTabs.add(tabId);
      updateBadge(tabId, true);
      return { success: true };
    }

    // Fallback only: an ID minted in the service worker is not usable from the isolated
    // offscreen document (see startTabCapture()).
    const streamId = providedStreamId || await chrome.tabCapture.getMediaStreamId({
      targetTabId: tabId
    });

    if (!streamId) {
      console.error('[CDSP] Failed to obtain mediaStreamId for tab', tabId);
      return { success: false, error: 'Failed to obtain media stream ID' };
    }

    // Forward the streamId to the offscreen document to instantiate Web Audio
    const response = await new Promise((resolve) => {
      chrome.runtime.sendMessage({
        target: 'offscreen',
        type: 'START_TAB_CAPTURE',
        tabId: tabId,
        streamId: streamId
      }, (resp) => {
        if (chrome.runtime.lastError) {
          resolve({ success: false, error: chrome.runtime.lastError.message });
        } else {
          resolve(resp);
        }
      });
    });

    if (response && response.success) {
      activeCapturedTabs.add(tabId);
      updateBadge(tabId, true);
      return { success: true };
    } else {
      return { success: false, error: response?.error || 'Offscreen initialization failed' };
    }
  } catch (err) {
    console.error('[CDSP] Error starting tab capture:', err);
    return { success: false, error: err.message };
  }
}

/**
 * Stops audio capture on a target tab.
 */
async function stopTabCapture(tabId) {
  try {
    if (await chrome.offscreen.hasDocument()) {
      await chrome.runtime.sendMessage({
        target: 'offscreen',
        type: 'STOP_TAB_CAPTURE',
        tabId: tabId
      });
    }
    activeCapturedTabs.delete(tabId);
    updateBadge(tabId, false);
    return { success: true };
  } catch (err) {
    console.error('[CDSP] Error stopping tab capture:', err);
    return { success: false, error: err.message };
  }
}

/**
 * Updates extension action badge indicator.
 */
function updateBadge(tabId, active) {
  if (active) {
    chrome.action.setBadgeText({ tabId, text: 'ON' });
    chrome.action.setBadgeBackgroundColor({ tabId, color: '#10b981' }); // emerald green
  } else {
    chrome.action.setBadgeText({ tabId, text: '' });
  }
}

// Clean up capture state when a tab is closed or navigated
chrome.tabs.onRemoved.addListener((tabId) => {
  if (activeCapturedTabs.has(tabId)) {
    stopTabCapture(tabId);
  }
});

chrome.tabs.onUpdated.addListener((tabId, changeInfo) => {
  if (activeCapturedTabs.has(tabId) && changeInfo.status === 'loading') {
    // Stop DSP when tab navigates to a new page
    stopTabCapture(tabId);
  }
});

// Clicking the extension icon opens or focuses the CDSP Studio tab
chrome.action.onClicked.addListener(async (tab) => {
  const studioUrl = chrome.runtime.getURL('ui/studio/cdsp-studio.html');
  const existingTabs = await chrome.tabs.query({ url: studioUrl });
  if (existingTabs.length > 0) {
    await chrome.tabs.update(existingTabs[0].id, { active: true });
    if (existingTabs[0].windowId) {
      await chrome.windows.update(existingTabs[0].windowId, { focused: true });
    }
  } else {
    await chrome.tabs.create({ url: studioUrl });
  }
});

/**
 * Finds the most appropriate web tab to capture (e.g. audible tab or active web tab).
 *
 * @param {number=} tabId Explicit target; returned as-is when given.
 * @param {number=} excludeTabId Tab to never pick (the requesting Studio tab). Without the
 *     "tabs" permission tab.url is usually undefined, so the URL filter alone cannot exclude
 *     extension pages.
 */
async function resolveTargetTabId(tabId, excludeTabId) {
  if (tabId) return tabId;
  const isCandidate = (t) => t.id && t.id !== excludeTabId &&
    !t.url?.startsWith('chrome-extension://') && !t.url?.startsWith('chrome://');
  try {
    // 1. Check for any audible non-extension tab
    const audibleTabs = await chrome.tabs.query({ audible: true });
    for (const t of audibleTabs) {
      if (isCandidate(t)) return t.id;
    }
    // 2. Check for active tab in non-extension windows
    const activeTabs = await chrome.tabs.query({ active: true });
    for (const t of activeTabs) {
      if (isCandidate(t)) return t.id;
    }
    // 3. Check for any normal web page tab
    const allTabs = await chrome.tabs.query({});
    for (const t of allTabs) {
      if (isCandidate(t)) return t.id;
    }
  } catch (e) {
    console.warn('[CDSP] Tab query error:', e);
  }
  return null;
}

// Track tab activation to remember the latest target tab
chrome.tabs.onActivated.addListener(async (activeInfo) => {
  try {
    const tab = await chrome.tabs.get(activeInfo.tabId);
    if (tab && tab.url && !tab.url.startsWith('chrome-extension://') && !tab.url.startsWith('chrome://')) {
      chrome.storage.local.set({ lastTargetTabId: activeInfo.tabId });
    }
  } catch (e) {}
});

// Message listener for popup and studio UI commands
chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
  if (message.target === 'background') {
    if (message.type === 'GET_STATUS') {
      const isCaptured = message.tabId !== undefined
        ? activeCapturedTabs.has(message.tabId)
        : (activeCapturedTabs.size > 0);
      sendResponse({ isCaptured, activeTabIds: Array.from(activeCapturedTabs) });
      return false;
    }

    // Lets an extension page pick the target tab and mint the stream ID itself (see
    // startTabCapture() for why the service worker cannot mint a usable one).
    if (message.type === 'RESOLVE_CAPTURE_TARGET') {
      (async () => {
        const tabId = await resolveTargetTabId(message.tabId, sender.tab?.id);
        sendResponse({ tabId, isCaptured: !!tabId && activeCapturedTabs.has(tabId) });
      })();
      return true;
    }

    if (message.type === 'START_CAPTURE') {
      (async () => {
        let tabId = message.tabId;
        if (!tabId) {
          tabId = await resolveTargetTabId(undefined, sender.tab?.id);
        }
        if (!tabId) {
          sendResponse({ success: false, isCaptured: false, error: 'No captureable tab found' });
          return;
        }
        const res = await startTabCapture(tabId, message.streamId);
        sendResponse({ ...res, isCaptured: activeCapturedTabs.size > 0, tabId });
      })();
      return true;
    }

    if (message.type === 'STOP_CAPTURE') {
      (async () => {
        if (message.tabId) {
          await stopTabCapture(message.tabId);
        } else {
          for (const tid of Array.from(activeCapturedTabs)) {
            await stopTabCapture(tid);
          }
        }
        sendResponse({ success: true, isCaptured: false });
      })();
      return true;
    }

    if (message.type === 'TOGGLE_CAPTURE') {
      (async () => {
        if (activeCapturedTabs.size > 0) {
          for (const tid of Array.from(activeCapturedTabs)) {
            await stopTabCapture(tid);
          }
          sendResponse({ success: true, isCaptured: false });
        } else {
          let tabId = message.tabId;
          if (!tabId) {
            tabId = await resolveTargetTabId(undefined, sender.tab?.id);
          }
          if (!tabId) {
            sendResponse({ success: false, isCaptured: false, error: 'No captureable tab found' });
            return;
          }
          const res = await startTabCapture(tabId, message.streamId);
          sendResponse({ ...res, isCaptured: activeCapturedTabs.size > 0, tabId });
        }
      })();
      return true; // Keep message channel open for async response
    }
  }
  return false;
});
