import React, { useState, useEffect } from 'react';
import { errorLogger, ErrorLogEntry } from '../utils/errorLogger';

export const DebugPanel: React.FC = () => {
  const [isOpen, setIsOpen] = useState(false);
  const [errorLogs, setErrorLogs] = useState<ErrorLogEntry[]>([]);
  const [swLogs, setSWLogs] = useState<any[]>([]);

  useEffect(() => {
    if (isOpen) {
      // Load stored error logs
      const logs = errorLogger.getStoredErrors();
      setErrorLogs(logs);

      // Get service worker logs if available
      if ('serviceWorker' in navigator && navigator.serviceWorker.controller) {
        const messageChannel = new MessageChannel();
        messageChannel.port1.onmessage = (event) => {
          if (event.data.type === 'SW_LOGS') {
            setSWLogs(event.data.logs || []);
          }
        };
        navigator.serviceWorker.controller.postMessage(
          { type: 'GET_SW_LOGS' },
          [messageChannel.port2]
        );
      }
    }
  }, [isOpen]);

  const clearLogs = () => {
    errorLogger.clearStoredErrors();
    setErrorLogs([]);
    
    // Clear service worker logs
    if ('serviceWorker' in navigator && navigator.serviceWorker.controller) {
      const messageChannel = new MessageChannel();
      navigator.serviceWorker.controller.postMessage(
        { type: 'CLEAR_SW_LOGS' },
        [messageChannel.port2]
      );
      setSWLogs([]);
    }
  };

  const exportLogs = () => {
    const allLogs = {
      timestamp: new Date().toISOString(),
      clientLogs: errorLogs,
      serviceworkerLogs: swLogs,
      deviceInfo: {
        userAgent: navigator.userAgent,
        viewport: {
          width: window.innerWidth,
          height: window.innerHeight,
          visualViewport: window.visualViewport ? {
            width: window.visualViewport.width,
            height: window.visualViewport.height,
            scale: window.visualViewport.scale,
          } : null,
        },
        screen: {
          width: window.screen.width,
          height: window.screen.height,
          pixelRatio: window.devicePixelRatio,
        },
        memory: (performance as any).memory ? {
          usedJSHeapSize: (performance as any).memory.usedJSHeapSize,
          totalJSHeapSize: (performance as any).memory.totalJSHeapSize,
          jsHeapSizeLimit: (performance as any).memory.jsHeapSizeLimit,
        } : null,
      },
    };

    const blob = new Blob([JSON.stringify(allLogs, null, 2)], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `foolish-debug-logs-${new Date().toISOString().split('T')[0]}.json`;
    a.click();
    URL.revokeObjectURL(url);
  };

  if (!isOpen) {
    return (
      <button
        onClick={() => setIsOpen(true)}
        style={{
          position: 'fixed',
          top: '10px',
          right: '10px',
          zIndex: 10000,
          backgroundColor: '#ff6b6b',
          color: 'white',
          border: 'none',
          borderRadius: '50%',
          width: '50px',
          height: '50px',
          fontSize: '20px',
          cursor: 'pointer',
          opacity: 0.7,
        }}
        title="Open Debug Panel"
      >
        🐛
      </button>
    );
  }

  return (
    <div style={{
      position: 'fixed',
      top: '10px',
      right: '10px',
      width: '400px',
      maxHeight: '80vh',
      backgroundColor: 'white',
      border: '2px solid #ccc',
      borderRadius: '8px',
      zIndex: 10000,
      boxShadow: '0 4px 12px rgba(0,0,0,0.3)',
      overflow: 'hidden',
      fontFamily: 'monospace',
      fontSize: '12px',
    }}>
      {/* Header */}
      <div style={{
        backgroundColor: '#333',
        color: 'white',
        padding: '8px 12px',
        display: 'flex',
        justifyContent: 'space-between',
        alignItems: 'center',
      }}>
        <span>🐛 Debug Panel</span>
        <button
          onClick={() => setIsOpen(false)}
          style={{
            backgroundColor: 'transparent',
            color: 'white',
            border: 'none',
            cursor: 'pointer',
            fontSize: '16px',
          }}
        >
          ✕
        </button>
      </div>

      {/* Controls */}
      <div style={{ padding: '8px', borderBottom: '1px solid #eee' }}>
        <button
          onClick={clearLogs}
          style={{
            marginRight: '8px',
            padding: '4px 8px',
            backgroundColor: '#ff6b6b',
            color: 'white',
            border: 'none',
            borderRadius: '4px',
            cursor: 'pointer',
          }}
        >
          Clear Logs
        </button>
        <button
          onClick={exportLogs}
          style={{
            padding: '4px 8px',
            backgroundColor: '#4dabf7',
            color: 'white',
            border: 'none',
            borderRadius: '4px',
            cursor: 'pointer',
          }}
        >
          Export Logs
        </button>
      </div>

      {/* Content */}
      <div style={{ maxHeight: '60vh', overflow: 'auto', padding: '8px' }}>
        <h4 style={{ margin: '0 0 8px 0' }}>
          Client Errors ({errorLogs.length})
        </h4>
        {errorLogs.length === 0 ? (
          <p style={{ color: '#666', margin: '0 0 16px 0' }}>No client errors logged</p>
        ) : (
          <div style={{ marginBottom: '16px' }}>
            {errorLogs.slice(-5).reverse().map((log, index) => (
              <div key={index} style={{
                border: '1px solid #ddd',
                borderRadius: '4px',
                padding: '8px',
                marginBottom: '8px',
                backgroundColor: log.type === 'javascript_error' ? '#ffebee' : '#fff',
              }}>
                <div style={{ fontWeight: 'bold', marginBottom: '4px' }}>
                  {log.type} - {log.context}
                </div>
                <div style={{ color: '#666', marginBottom: '4px' }}>
                  {new Date(log.timestamp).toLocaleString()}
                </div>
                <div style={{ color: '#d32f2f' }}>
                  {log.error.name}: {log.error.message}
                </div>
                {log.iosInfo && (
                  <div style={{ marginTop: '4px', fontSize: '10px', color: '#666' }}>
                    iOS: Safari {log.iosInfo.safariVersion}, Standalone: {log.iosInfo.standalone ? 'Yes' : 'No'}
                  </div>
                )}
              </div>
            ))}
          </div>
        )}

        <h4 style={{ margin: '0 0 8px 0' }}>
          Service Worker Errors ({swLogs.length})
        </h4>
        {swLogs.length === 0 ? (
          <p style={{ color: '#666', margin: 0 }}>No service worker errors logged</p>
        ) : (
          <div>
            {swLogs.slice(-5).reverse().map((log, index) => (
              <div key={index} style={{
                border: '1px solid #ddd',
                borderRadius: '4px',
                padding: '8px',
                marginBottom: '8px',
                backgroundColor: '#e3f2fd',
              }}>
                <div style={{ fontWeight: 'bold', marginBottom: '4px' }}>
                  {log.context}
                </div>
                <div style={{ color: '#666', marginBottom: '4px' }}>
                  {new Date(log.timestamp).toLocaleString()}
                </div>
                <div style={{ color: '#1976d2' }}>
                  {log.error.name}: {log.error.message}
                </div>
              </div>
            ))}
          </div>
        )}
      </div>
    </div>
  );
};