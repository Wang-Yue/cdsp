// Polyfill performance.now in AudioWorkletGlobalScope if missing
if (typeof performance === 'undefined') {
  globalThis.performance = {
    now: function() {
      return Date.now();
    }
  };
} else if (typeof performance.now !== 'function') {
  performance.now = function() {
    return Date.now();
  };
}
