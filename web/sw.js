// Service worker for installing the page as an app. It caches nothing: the game files are
// large and versioned by the server, so every request goes to the network as usual.
'use strict';

self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (event) => event.waitUntil(self.clients.claim()));
// older Chromium versions only offer installation when a fetch handler exists
self.addEventListener('fetch', () => {});
