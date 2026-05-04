// edevkit docs hub — service worker
// Strategy:
//   · /static/* and the hub root  → cache-first, revalidate in background
//   · /docs/*.html                → stale-while-revalidate
//   · /api/search-index           → stale-while-revalidate
//   · everything else             → network only
//
// Bump CACHE_VERSION when assets change in a backwards-incompatible way.

const CACHE_VERSION = 'edevkit-v1';
const STATIC_CACHE  = `${CACHE_VERSION}-static`;
const DOC_CACHE     = `${CACHE_VERSION}-docs`;
const API_CACHE     = `${CACHE_VERSION}-api`;

// Pre-cache the hub shell so first offline visit works.
const PRECACHE_URLS = [
  '/',
  '/static/style.css',
  '/static/tailwind.css',
  '/static/script.js',
  '/static/syntax.css',
  '/static/syntax.js',
  '/static/favicon.svg',
  '/static/manifest.webmanifest',
];

self.addEventListener('install', (event) => {
  event.waitUntil(
    caches.open(STATIC_CACHE).then((cache) => cache.addAll(PRECACHE_URLS))
      .catch(() => {})  // never fail install if a single resource is missing
      .then(() => self.skipWaiting())
  );
});

self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches.keys().then((keys) =>
      Promise.all(
        keys
          .filter((k) => !k.startsWith(CACHE_VERSION))
          .map((k) => caches.delete(k))
      )
    ).then(() => self.clients.claim())
  );
});

function isStaticAsset(url) {
  return url.pathname.startsWith('/static/');
}
function isDoc(url) {
  return url.pathname.startsWith('/docs/');
}
function isApi(url) {
  return url.pathname.startsWith('/api/');
}
function isHubShell(url) {
  return url.pathname === '/' || url.pathname === '/index.html';
}

// Cache-first: serve from cache, fall back to network, update cache async.
async function cacheFirst(request, cacheName) {
  const cache = await caches.open(cacheName);
  const cached = await cache.match(request);
  if (cached) {
    // Refresh in background, ignore failures.
    fetch(request).then((res) => res.ok && cache.put(request, res.clone())).catch(() => {});
    return cached;
  }
  try {
    const res = await fetch(request);
    if (res.ok) cache.put(request, res.clone());
    return res;
  } catch (e) {
    return cached || Response.error();
  }
}

// Stale-while-revalidate: return cache immediately, update cache in background.
async function staleWhileRevalidate(request, cacheName) {
  const cache = await caches.open(cacheName);
  const cached = await cache.match(request);
  const networkPromise = fetch(request)
    .then((res) => { if (res.ok) cache.put(request, res.clone()); return res; })
    .catch(() => null);
  return cached || (await networkPromise) || Response.error();
}

self.addEventListener('fetch', (event) => {
  const { request } = event;
  if (request.method !== 'GET') return;
  const url = new URL(request.url);
  if (url.origin !== location.origin) return;

  if (isStaticAsset(url) || isHubShell(url)) {
    event.respondWith(cacheFirst(request, STATIC_CACHE));
    return;
  }
  if (isDoc(url)) {
    event.respondWith(staleWhileRevalidate(request, DOC_CACHE));
    return;
  }
  if (isApi(url)) {
    event.respondWith(staleWhileRevalidate(request, API_CACHE));
    return;
  }
});
