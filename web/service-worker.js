/* RGRV offline cache. Every path is relative to the page root. */
const CACHE_NAME = 'rgrv-v2';
const urlsToCache = [
  './',
  './index.html',
  './style.css',
  './app.js',
  './manifest.json',
  './favicon.svg',
  './icon-192x192.png',
  './icon-512x512.png',
  './apple-touch-icon.png',
  './wasm/rgrv.js',
  './wasm/rgrv.wasm'
];

// Install: cache the app.
self.addEventListener('install', event => {
  event.waitUntil(
    caches.open(CACHE_NAME)
      .then(cache => cache.addAll(
        urlsToCache.map(url => new Request(url, { cache: 'reload' }))
      ))
      .catch(err => console.error('Cache install failed:', err))
  );
  self.skipWaiting();
});

// Activate: drop old caches.
self.addEventListener('activate', event => {
  event.waitUntil(
    caches.keys().then(names => Promise.all(
      names
        .filter(name => name !== CACHE_NAME)
        .map(name => caches.delete(name))
    ))
  );
  self.clients.claim();
});

// Fetch: cache first, then network, and cache what comes back.
self.addEventListener('fetch', event => {
  if (event.request.method !== 'GET') return;

  event.respondWith(
    caches.match(event.request).then(cached => {
      if (cached) return cached;

      return fetch(event.request).then(response => {
        if (!response || response.status !== 200 || response.type === 'error') {
          return response;
        }
        const copy = response.clone();
        caches.open(CACHE_NAME).then(cache => cache.put(event.request, copy));
        return response;
      });
    })
  );
});
