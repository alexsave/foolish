// IndexedDB-based texture caching for persistent storage across sessions
// Stores blob data efficiently without consuming JS heap memory

const DB_NAME = 'TextureCacheDB';
const DB_VERSION = 1;
const STORE_NAME = 'textures';

// Version keys - increment these to invalidate old cached textures
const TEXTURE_VERSIONS: Record<TextureType, string> = {
  fern: 'v1',
  wood: 'v2',
  wool: 'v1',
  soviet: 'v1',
  concrete: 'v1',
};

type TextureType = 'fern' | 'wood' | 'wool' | 'soviet' | 'concrete';

// Initialize IndexedDB
function openDB(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open(DB_NAME, DB_VERSION);

    request.onerror = () => reject(request.error);
    request.onsuccess = () => resolve(request.result);

    request.onupgradeneeded = (event) => {
      const db = (event.target as IDBOpenDBRequest).result;
      if (!db.objectStoreNames.contains(STORE_NAME)) {
        db.createObjectStore(STORE_NAME, { keyPath: 'id' });
      }
    };
  });
}

// iOS Safari has a long-standing bug where indexedDB.open() can hang forever
// right after page load — no success, no error. Anything awaiting the cache
// would then never proceed to generation (textures silently never appear, no
// console output). Race every cache read against a deadline and fall back to
// regeneration instead.
function withDeadline<T>(promise: Promise<T>, ms: number, fallback: T): Promise<T> {
  return Promise.race([
    promise,
    new Promise<T>((resolve) => setTimeout(() => resolve(fallback), ms)),
  ]);
}

// Get texture from cache
export async function getCachedTexture(type: TextureType): Promise<string | null> {
  return withDeadline(getCachedTextureInner(type), 1500, null);
}

async function getCachedTextureInner(type: TextureType): Promise<string | null> {
  try {
    const db = await openDB();
    const transaction = db.transaction([STORE_NAME], 'readonly');
    const store = transaction.objectStore(STORE_NAME);
    const key = `${type}_${TEXTURE_VERSIONS[type]}`;

    return new Promise((resolve, reject) => {
      const request = store.get(key);

      request.onsuccess = () => {
        const result = request.result;
        if (result && result.blob) {
          // Convert stored blob back to blob URL
          const blobUrl = URL.createObjectURL(result.blob);
          resolve(blobUrl);
        } else {
          resolve(null);
        }
      };

      request.onerror = () => {
        console.warn('Failed to get cached texture:', request.error);
        resolve(null);
      };
    });
  } catch (error) {
    console.warn('IndexedDB not available:', error);
    return null;
  }
}

// Save texture to cache
export async function setCachedTexture(type: TextureType, blobUrl: string): Promise<void> {
  try {
    // Fetch the blob from the blob URL
    const response = await fetch(blobUrl);
    const blob = await response.blob();

    const db = await openDB();
    const transaction = db.transaction([STORE_NAME], 'readwrite');
    const store = transaction.objectStore(STORE_NAME);
    const key = `${type}_${TEXTURE_VERSIONS[type]}`;

    const data = {
      id: key,
      blob: blob,
      timestamp: Date.now(),
    };

    return new Promise((resolve, reject) => {
      const request = store.put(data);

      request.onsuccess = () => {
        console.log(`Cached ${type} texture to IndexedDB`);
        resolve();
      };

      request.onerror = () => {
        console.warn('Failed to cache texture:', request.error);
        resolve(); // Don't reject, just log
      };
    });
  } catch (error) {
    console.warn('Failed to cache texture:', error);
    // Don't throw, just continue without caching
  }
}

// Clear all cached textures (useful for debugging)
export async function clearTextureCache(): Promise<void> {
  try {
    const db = await openDB();
    const transaction = db.transaction([STORE_NAME], 'readwrite');
    const store = transaction.objectStore(STORE_NAME);

    return new Promise((resolve, reject) => {
      const request = store.clear();

      request.onsuccess = () => {
        console.log('Cleared texture cache');
        resolve();
      };

      request.onerror = () => {
        console.warn('Failed to clear cache:', request.error);
        resolve();
      };
    });
  } catch (error) {
    console.warn('Failed to clear cache:', error);
  }
}
