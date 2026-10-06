/**
 * @file service_worker.js
 * @brief Chrome Extension Background Service Worker (Manifest V3)
 *
 * The DSP engine and its audio graph live in the CDSP Studio tab (the Qt WebAssembly app runs
 * the engine threads and the AudioWorklet device itself). The service worker only opens the
 * Studio window, picks which tab to capture, and shows a badge on the captured tab.
 */

// Captured tab ID -> Studio tab ID that captures it.
const capturedTabs = new Map();

/**
 * Updates extension action badge indicator.
 */
function updateBadge(tabId, active) {
  if (active) {
    chrome.action.setBadgeText({ tabId, text: 'ON' });
    chrome.action.setBadgeBackgroundColor({ tabId, color: '#10b981' }); // emerald green
  } else {
    chrome.action.setBadgeText({ tabId, text: '' }).catch(() => {});
  }
}

function setCaptureState(tabId, studioTabId, active) {
  if (active) {
    capturedTabs.set(tabId, studioTabId);
  } else {
    capturedTabs.delete(tabId);
  }
  updateBadge(tabId, active);
}

// A closed Studio tab takes its capture (and its background notice) with it.
chrome.tabs.onRemoved.addListener((tabId) => {
  capturedTabs.delete(tabId);
  for (const [capturedTabId, studioTabId] of Array.from(capturedTabs)) {
    if (studioTabId === tabId) {
      setCaptureState(capturedTabId, studioTabId, false);
      chrome.notifications.clear(BACKGROUND_NOTICE_ID).catch(() => {});
    }
  }
});

// Clicking the extension icon opens or focuses the CDSP Studio window. It opens as a 'popup'
// window (no tab strip, address bar or toolbar) so Studio looks like a standalone app; its size
// and position are remembered across launches.
const STUDIO_DEFAULT_BOUNDS = { width: 1280, height: 820 };

async function showStudio() {
  chrome.notifications.clear(BACKGROUND_NOTICE_ID).catch(() => {});
  const studioUrl = chrome.runtime.getURL('ui/studio/cdsp-studio.html');
  // chrome.tabs.query({ url }) cannot match URLs without the "tabs" permission, so it never
  // finds Studio; runtime.getContexts() lists the extension's own pages without it.
  const contexts = await chrome.runtime.getContexts({ contextTypes: ['TAB'], documentUrls: [studioUrl] });
  const existing = contexts.find((c) => c.tabId >= 0 && c.windowId >= 0);
  if (existing) {
    await chrome.tabs.update(existing.tabId, { active: true });
    // Studio may have been minimized on close to keep the DSP running; bring it back.
    const win = await chrome.windows.get(existing.windowId);
    const update = { focused: true };
    if (win.state === 'minimized') update.state = 'normal';
    await chrome.windows.update(existing.windowId, update);
  } else {
    const { studioBounds } = await chrome.storage.local.get('studioBounds');
    const createStudio = (bounds) =>
      chrome.windows.create({ url: studioUrl, type: 'popup', focused: true, ...bounds });
    let win;
    try {
      win = await createStudio(studioBounds || STUDIO_DEFAULT_BOUNDS);
    } catch (e) {
      // Remembered bounds can be invalid (e.g. that monitor is gone).
      win = await createStudio(STUDIO_DEFAULT_BOUNDS);
    }
    await chrome.storage.session.set({ studioWindowId: win.id, studioMode: 'main' });
  }
}

chrome.action.onClicked.addListener(() => showStudio());

// Shown when Studio was minimized on close (the user chose "Cancel" in the "Leave site?"
// prompt): confirms the DSP keeps running. On ChromeOS it sits in the system tray.
const BACKGROUND_NOTICE_ID = 'cdsp-background';

function showBackgroundNotice() {
  chrome.notifications.create(BACKGROUND_NOTICE_ID, {
    type: 'basic',
    iconUrl: chrome.runtime.getURL('ui/icons/icon128.png'),
    title: 'CDSP is running in the background',
    message: 'Studio was minimized and audio processing continues. Click here or the CDSP icon to reopen Studio.',
    buttons: [{ title: 'Show Studio' }],
    priority: 0
  }).catch((e) => console.warn('[CDSP] Background notification failed:', e));
}

chrome.notifications.onClicked.addListener((id) => {
  if (id === BACKGROUND_NOTICE_ID) showStudio();
});
chrome.notifications.onButtonClicked.addListener((id) => {
  if (id === BACKGROUND_NOTICE_ID) showStudio();
});

// Remember where the user puts the Studio window, separately for the main window and the mini
// player (the Studio page sets studioMode and resizes the window when it switches).
chrome.windows.onBoundsChanged.addListener(async (win) => {
  const { studioWindowId, studioMode } = await chrome.storage.session.get(['studioWindowId', 'studioMode']);
  if (win.id === studioWindowId && win.state === 'normal') {
    const { left, top, width, height } = win;
    const key = studioMode === 'mini' ? 'studioMiniBounds' : 'studioBounds';
    chrome.storage.local.set({ [key]: { left, top, width, height } });
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

// Messages from the Studio page
chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
  if (message.target !== 'background') return false;

  // The Studio page captures the tab itself: chrome.tabCapture.getMediaStreamId() binds the
  // stream ID to the caller's process, so it must be minted (and consumed) in the Studio page.
  if (message.type === 'RESOLVE_CAPTURE_TARGET') {
    (async () => {
      const tabId = await resolveTargetTabId(message.tabId, sender.tab?.id);
      sendResponse({ tabId });
    })();
    return true;
  }

  if (message.type === 'CAPTURE_STATE') {
    setCaptureState(message.tabId, sender.tab?.id, !!message.active);
    sendResponse({ success: true });
    return false;
  }

  if (message.type === 'BACKGROUND_NOTICE') {
    if (message.show) {
      showBackgroundNotice();
    } else {
      chrome.notifications.clear(BACKGROUND_NOTICE_ID).catch(() => {});
    }
    sendResponse({ success: true });
    return false;
  }

  return false;
});
