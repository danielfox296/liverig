// Offline shell: the rig keeps working at a gig with no signal.
const CACHE = 'liverig-v1';
const FILES = ['./', 'index.html', 'style.css', 'icon.png', 'manifest.webmanifest',
               'js/app.js', 'js/model.js', 'js/foot.js', 'js/data.js', 'js/dsp.js', 'js/engine.js', 'js/worklet-main.js'];
self.addEventListener ('install', (e) => e.waitUntil (caches.open (CACHE).then ((c) => c.addAll (FILES)).then (() => self.skipWaiting ())));
self.addEventListener ('activate', (e) => e.waitUntil (caches.keys ().then ((ks) => Promise.all (ks.filter ((k) => k !== CACHE).map ((k) => caches.delete (k)))).then (() => self.clients.claim ())));
// Network first, so an update lands as soon as there is signal; the cache is the fallback.
self.addEventListener ('fetch', (e) =>
{
    if (e.request.method !== 'GET') return;
    e.respondWith (fetch (e.request).then ((r) => { const copy = r.clone (); caches.open (CACHE).then ((c) => c.put (e.request, copy)); return r; })
                                    .catch (() => caches.match (e.request)));
});
