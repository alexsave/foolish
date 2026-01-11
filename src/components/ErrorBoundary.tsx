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
        <div className="error-boundary">
          <h2 className="error-boundary__title">🚨 Something went wrong</h2>
          <p className="error-boundary__info"><strong>Error ID:</strong> {this.state.errorId}</p>
          <p className="error-boundary__info"><strong>Context:</strong> {this.props.context || 'Unknown'}</p>
          <p className="error-boundary__info"><strong>Time:</strong> {new Date().toLocaleString()}</p>
          
          {this.state.error && (
            <details className="error-boundary__details">
              <summary className="error-boundary__summary">
                Error Details (click to expand)
              </summary>
              <pre className="error-boundary__stack">
                {this.state.error.message}
                {'\n\n'}
                {this.state.error.stack}
              </pre>
            </details>
          )}
          
          <div className="error-boundary__actions">
            <button onClick={this.handleRetry} className="btn-error btn-error--retry">
              Try Again
            </button>
            <button onClick={this.handleReload} className="btn-error btn-error--reload">
              Reload Page
            </button>
          </div>
          
          <div className="error-boundary__footer">
            <p>This error has been logged for debugging. Error ID: {this.state.errorId}</p>
            <p>If this keeps happening, please contact support with the Error ID.</p>
          </div>
        </div>
      );
    }

    return this.props.children;
  }
}