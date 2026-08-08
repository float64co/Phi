/*
 * library_ws_stub.js
 * Supplementary JS library for Emscripten build.
 * The actual WebSocket work is done by Emscripten's built-in
 * emscripten_websocket_* API. This file just exposes the
 * net_connect_js wrapper so index.html can call it after the
 * module initialises.
 */
mergeInto(LibraryManager.library, {
  // Intentionally empty — net_connect_js is defined in main.c
  // and exported via EXPORTED_FUNCTIONS. This file exists as a
  // placeholder for any future custom JS<->C interop.
  js_noop__sig: 'v',
  js_noop: function() {}
});
