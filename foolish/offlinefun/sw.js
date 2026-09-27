const CACHE_NAME = 'foolish-cards-v2';
const STATIC_CACHE_NAME = 'foolish-static-v2';
const DYNAMIC_CACHE_NAME = 'foolish-dynamic-v2';

const staticAssets = [
  '/',
  '/manifest.json',
  '/favicon.ico',
  '/favicon-16x16.png',
  '/favicon-32x32.png',
  '/khokhloma-pattern.png',
  '/android-chrome-192x192.png',
  '/android-chrome-512x512.png',
  '/apple-touch-icon.png',
  '/robots.txt'
];

// Enhanced logging function for service worker
function logSWError(context, error, additionalInfo = {}) {
  const timestamp = new Date().toISOString();
  const logEntry = {
    timestamp,
    context: `SW - ${context}`,
    error: {
      name: error?.name || 'Unknown',
      message: error?.message || String(error),
      stack: error?.stack,
    },
    additionalInfo,
    userAgent: navigator.userAgent,
    isIOS: /iPad|iPhone|iPod/.test(navigator.userAgent),
  };
  
  console.error('🚨 Service Worker Error:', logEntry);
  
  // In service worker, we can't use localStorage directly, but we can post message to clients
  // or use IndexedDB. For now, console.error will show in dev tools.
}

// Install event - cache core assets
self.addEventListener('install', (event) => {
  event.waitUntil(
    caches.open(STATIC_CACHE_NAME)
      .then((cache) => {
        console.log('SW: Caching static assets');
        // Cache assets individually to avoid failure on one blocking all
        return Promise.allSettled(
          staticAssets.map(asset => {
            return cache.add(asset).catch(error => {
              console.log(`SW: Failed to cache ${asset}:`, error);
              return null; // Don't fail the whole operation
            });
          })
        );
      })
      .then(() => {
        console.log('SW: Static assets caching completed');
      })
      .catch((error) => {
        console.log('SW: Install failed:', error);
      })
  );
  self.skipWaiting();
});

// Fetch event - comprehensive offline strategy
self.addEventListener('fetch', (event) => {
  try {
    const { request } = event;
    const url = new URL(request.url);

    // Log fetch requests for debugging (only for app domain)
    if (url.origin === location.origin) {
      console.log('🌐 SW Fetch:', request.method, request.url);
    }

    // Handle navigation requests (HTML pages)
    if (request.mode === 'navigate') {
      event.respondWith(
        caches.match('/').then((response) => {
          if (response) {
            console.log('📄 Navigation served from cache');
            return response;
          }
          console.log('📄 Navigation served from network');
          return fetch(request).catch((error) => {
            logSWError('Fetch - Navigation', error, {
              url: request.url,
              mode: request.mode,
            });
            // Return a basic offline page
            return new Response('<!DOCTYPE html><html><head><title>Offline</title></head><body><h1>App is offline</h1><p>Please check your connection and try again.</p></body></html>', {
              headers: { 'Content-Type': 'text/html' }
            });
          });
        }).catch((error) => {
          logSWError('Fetch - Navigation Cache', error, {
            url: request.url,
          });
          return fetch(request);
        })
      );
      return;
    }

    // Handle static assets (JS, CSS, images)
    if (url.origin === location.origin) {
      event.respondWith(
        caches.match(request).then((response) => {
          if (response) {
            console.log('📦 Asset served from cache:', request.url);
            return response;
          }
          
          console.log('🌐 Asset fetched from network:', request.url);
          
          // Try to fetch and cache for future use
          return fetch(request).then((fetchResponse) => {
            // Only cache successful responses
            if (fetchResponse.status === 200) {
              const responseClone = fetchResponse.clone();
              const cacheName = request.url.includes('/static/') ? 
                DYNAMIC_CACHE_NAME : STATIC_CACHE_NAME;
              
              caches.open(cacheName).then((cache) => {
                cache.put(request, responseClone);
              }).catch((cacheError) => {
                logSWError('Fetch - Cache Put', cacheError, {
                  url: request.url,
                  cacheName,
                });
              });
            } else {
              logSWError('Fetch - Non-200 Response', new Error(`HTTP ${fetchResponse.status}`), {
                url: request.url,
                status: fetchResponse.status,
                statusText: fetchResponse.statusText,
              });
            }
            return fetchResponse;
          }).catch((fetchError) => {
            logSWError('Fetch - Network Error', fetchError, {
              url: request.url,
              mode: request.mode,
              destination: request.destination,
            });
            
            // If fetch fails and it's a JS/CSS file, return a minimal response
            if (request.url.includes('.js')) {
              console.log('🔧 Serving fallback JS for:', request.url);
              return new Response('console.log("Offline - script not available");', {
                headers: { 'Content-Type': 'application/javascript' }
              });
            }
            if (request.url.includes('.css')) {
              console.log('🔧 Serving fallback CSS for:', request.url);
              return new Response('/* Offline - stylesheet not available */', {
                headers: { 'Content-Type': 'text/css' }
              });
            }
            // Return a basic error response for other failed requests
            return new Response('Offline', { status: 503, statusText: 'Service Unavailable' });
          });
        }).catch((cacheError) => {
          logSWError('Fetch - Cache Match', cacheError, {
            url: request.url,
          });
          // Fallback to network if cache fails
          return fetch(request);
        })
      );
    }
  } catch (error) {
    logSWError('Fetch - General Error', error, {
      url: event.request?.url,
      method: event.request?.method,
    });
    // Let the browser handle the request normally
  }
});

// Activate event - clean up old caches and take control
self.addEventListener('activate', (event) => {
  console.log('🚀 Service Worker activating...');
  
  event.waitUntil(
    Promise.all([
      // Clean up old caches
      caches.keys().then((cacheNames) => {
        console.log('🧹 Checking caches:', cacheNames);
        return Promise.all(
          cacheNames.map((cacheName) => {
            if (![STATIC_CACHE_NAME, DYNAMIC_CACHE_NAME].includes(cacheName)) {
              console.log('🗑️ Deleting old cache:', cacheName);
              return caches.delete(cacheName);
            }
          })
        );
      }).catch((error) => {
        logSWError('Activate - Cache Cleanup', error);
        return Promise.resolve();
      }),
      
      // Take control of all clients immediately
      self.clients.claim().then(() => {
        console.log('👑 Service Worker claimed all clients');
      }).catch((error) => {
        logSWError('Activate - Claim Clients', error);
      })
    ]).catch((error) => {
      logSWError('Activate - General Error', error);
    })
  );
});

// Handle errors in the service worker itself
self.addEventListener('error', (event) => {
  logSWError('Global SW Error', event.error, {
    filename: event.filename,
    lineno: event.lineno,
    colno: event.colno,
  });
});

// Handle unhandled promise rejections in service worker
self.addEventListener('unhandledrejection', (event) => {
  logSWError('SW Unhandled Rejection', event.reason);
});

// Add message handler for debugging
self.addEventListener('message', (event) => {
  console.log('📨 SW received message:', event.data);
  
  if (event.data.type === 'GET_SW_LOGS') {
    try {
      const logs = JSON.parse(localStorage.getItem('sw_error_logs') || '[]');
      event.ports[0].postMessage({ type: 'SW_LOGS', logs });
    } catch (error) {
      logSWError('Message - Get Logs', error);
      event.ports[0].postMessage({ type: 'SW_LOGS', logs: [], error: error.message });
    }
  }
  
  if (event.data.type === 'CLEAR_SW_LOGS') {
    try {
      localStorage.removeItem('sw_error_logs');
      event.ports[0].postMessage({ type: 'SW_LOGS_CLEARED' });
    } catch (error) {
      logSWError('Message - Clear Logs', error);
    }
  }
});

// Message event - for cache warming requests from the main thread
self.addEventListener('message', (event) => {
  if (event.data && event.data.type === 'WARM_CACHE') {
    event.waitUntil(
      caches.open(DYNAMIC_CACHE_NAME).then((cache) => {
        console.log('SW: Warming cache with URLs:', event.data.urls);
        return Promise.allSettled(
          event.data.urls.map((url) => {
            return fetch(url).then((response) => {
              if (response.status === 200) {
                console.log(`SW: Successfully cached: ${url}`);
                return cache.put(url, response);
              } else {
                console.log(`SW: Non-200 response for ${url}: ${response.status}`);
              }
            }).catch((error) => {
              console.log(`SW: Failed to cache ${url}:`, error.message);
            });
          })
        );
      }).then(() => {
        console.log('SW: Cache warming completed');
      })
    );
  }
});