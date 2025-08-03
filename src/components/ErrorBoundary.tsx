import React, { Component, ErrorInfo, ReactNode } from 'react';

interface Props {
  children: ReactNode;
  fallback?: ReactNode;
  context?: string;
}

interface State {
  hasError: boolean;
  error?: Error;
  errorInfo?: ErrorInfo;
  errorId?: string;
}

// Enhanced logging function
const logError = (error: Error, errorInfo: ErrorInfo, context: string, errorId: string) => {
  const timestamp = new Date().toISOString();
  const userAgent = navigator.userAgent;
  const url = window.location.href;
  const userId = localStorage.getItem('user_id') || 'anonymous';
  
  // Get device info
  const deviceInfo = {
    userAgent,
    platform: navigator.platform,
    language: navigator.language,
    cookieEnabled: navigator.cookieEnabled,
    onLine: navigator.onLine,
    screenWidth: window.screen.width,
    screenHeight: window.screen.height,
    windowWidth: window.innerWidth,
    windowHeight: window.innerHeight,
    pixelRatio: window.devicePixelRatio,
  };

  // Get iOS specific info
  const isIOS = /iPad|iPhone|iPod/.test(userAgent);
  const iosInfo = isIOS ? {
    standalone: (navigator as any).standalone,
    maxTouchPoints: navigator.maxTouchPoints,
    visualViewport: window.visualViewport ? {
      width: window.visualViewport.width,
      height: window.visualViewport.height,
      scale: window.visualViewport.scale,
    } : null,
  } : null;

  // Get memory info if available
  const memoryInfo = (performance as any).memory ? {
    usedJSHeapSize: (performance as any).memory.usedJSHeapSize,
    totalJSHeapSize: (performance as any).memory.totalJSHeapSize,
    jsHeapSizeLimit: (performance as any).memory.jsHeapSizeLimit,
  } : null;

  const errorLog = {
    errorId,
    timestamp,
    context,
    userId,
    url,
    error: {
      name: error.name,
      message: error.message,
      stack: error.stack,
    },
    errorInfo: {
      componentStack: errorInfo.componentStack,
    },
    deviceInfo,
    iosInfo,
    memoryInfo,
    gameState: {
      // Try to get game context if available
      gameId: window.location.pathname.split('/').pop(),
      storage: {
        localStorage: Object.keys(localStorage).reduce((acc, key) => {
          // Only include non-sensitive data
          if (!key.includes('password') && !key.includes('token')) {
            acc[key] = localStorage.getItem(key);
          }
          return acc;
        }, {} as Record<string, string | null>),
      },
    },
  };

  // Console log for immediate debugging
  console.error('🚨 ERROR BOUNDARY CAUGHT ERROR:', errorLog);

  // Also send to any external logging service you might have
  // Example: Send to your backend or external service
  try {
    // You can uncomment and modify this to send to your backend
    // fetch('/api/log-error', {
    //   method: 'POST',
    //   headers: { 'Content-Type': 'application/json' },
    //   body: JSON.stringify(errorLog),
    // }).catch(e => console.error('Failed to send error log:', e));
    
    // For now, also store in localStorage for later retrieval
    const storedErrors = JSON.parse(localStorage.getItem('error_logs') || '[]');
    storedErrors.push(errorLog);
    // Keep only last 10 errors to avoid storage bloat
    if (storedErrors.length > 10) {
      storedErrors.shift();
    }
    localStorage.setItem('error_logs', JSON.stringify(storedErrors));
  } catch (e) {
    console.error('Failed to store error log:', e);
  }
};

export class ErrorBoundary extends Component<Props, State> {
  constructor(props: Props) {
    super(props);
    this.state = { hasError: false };
  }

  static getDerivedStateFromError(error: Error): State {
    const errorId = `err_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`;
    return {
      hasError: true,
      error,
      errorId,
    };
  }

  componentDidCatch(error: Error, errorInfo: ErrorInfo) {
    const errorId = this.state.errorId || `err_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`;
    const context = this.props.context || 'Unknown';
    
    this.setState({
      error,
      errorInfo,
      errorId,
    });

    logError(error, errorInfo, context, errorId);
  }

  handleRetry = () => {
    this.setState({ hasError: false, error: undefined, errorInfo: undefined, errorId: undefined });
  };

  handleReload = () => {
    window.location.reload();
  };

  render() {
    if (this.state.hasError) {
      if (this.props.fallback) {
        return this.props.fallback;
      }

      return (
        <div style={{
          padding: '20px',
          margin: '20px',
          border: '2px solid #ff6b6b',
          borderRadius: '8px',
          backgroundColor: '#fff5f5',
          color: '#c92a2a',
          fontFamily: 'monospace',
          maxWidth: '800px',
          marginLeft: 'auto',
          marginRight: 'auto',
        }}>
          <h2>🚨 Something went wrong</h2>
          <p><strong>Error ID:</strong> {this.state.errorId}</p>
          <p><strong>Context:</strong> {this.props.context || 'Unknown'}</p>
          <p><strong>Time:</strong> {new Date().toLocaleString()}</p>
          
          {this.state.error && (
            <details style={{ marginTop: '10px' }}>
              <summary style={{ cursor: 'pointer', fontWeight: 'bold' }}>
                Error Details (click to expand)
              </summary>
              <pre style={{
                backgroundColor: '#f8f8f8',
                padding: '10px',
                borderRadius: '4px',
                overflow: 'auto',
                maxHeight: '200px',
                fontSize: '12px',
              }}>
                {this.state.error.message}
                {'\n\n'}
                {this.state.error.stack}
              </pre>
            </details>
          )}
          
          <div style={{ marginTop: '20px' }}>
            <button
              onClick={this.handleRetry}
              style={{
                marginRight: '10px',
                padding: '8px 16px',
                backgroundColor: '#4dabf7',
                color: 'white',
                border: 'none',
                borderRadius: '4px',
                cursor: 'pointer',
              }}
            >
              Try Again
            </button>
            <button
              onClick={this.handleReload}
              style={{
                padding: '8px 16px',
                backgroundColor: '#ff6b6b',
                color: 'white',
                border: 'none',
                borderRadius: '4px',
                cursor: 'pointer',
              }}
            >
              Reload Page
            </button>
          </div>
          
          <div style={{ marginTop: '15px', fontSize: '12px', opacity: 0.7 }}>
            <p>This error has been logged for debugging. Error ID: {this.state.errorId}</p>
            <p>If this keeps happening, please contact support with the Error ID.</p>
          </div>
        </div>
      );
    }

    return this.props.children;
  }
}