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
}

/**
 * Starts audio capture on a target tab.
 */
async function startTabCapture(tabId) {
  try {
    await ensureOffscreenDocument();

    // In Manifest V3, we obtain a stream ID targeting the specific tab
    const streamId = await chrome.tabCapture.getMediaStreamId({
      targetTabId: tabId
    });

    if (!streamId) {
      console.error('[CDSP] Failed to obtain mediaStreamId for tab', tabId);
      return { success: false, error: 'Failed to obtain media stream ID' };
    }

    // Forward the streamId to the offscreen document to instantiate Web Audio
    const response = await chrome.runtime.sendMessage({
      target: 'offscreen',
      type: 'START_TAB_CAPTURE',
      tabId: tabId,
      streamId: streamId
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

// Message listener for popup and studio UI commands
chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
  if (message.target === 'background') {
    if (message.type === 'GET_STATUS') {
      const isCaptured = activeCapturedTabs.has(message.tabId);
      sendResponse({ isCaptured });
      return false;
    }

    if (message.type === 'TOGGLE_CAPTURE') {
      const tabId = message.tabId;
      if (activeCapturedTabs.has(tabId)) {
        stopTabCapture(tabId).then(sendResponse);
      } else {
        startTabCapture(tabId).then(sendResponse);
      }
      return true; // Keep message channel open for async response
    }
  }
  return false;
});
