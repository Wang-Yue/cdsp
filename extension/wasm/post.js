if (typeof globalThis !== 'undefined') {
  globalThis.loadCdspWasm = loadCdspWasm;
}
if (typeof self !== 'undefined') {
  self.loadCdspWasm = loadCdspWasm;
}
