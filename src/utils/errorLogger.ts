// Comprehensive error logging utility for iOS crash debugging

export interface ErrorLogEntry {
  errorId: string;
  timestamp: string;
  type: 'javascript_error' | 'unhandled_rejection' | 'network_error' | 'ios_specific' | 'custom';
  context: string;
  userId?: string;
  url: string;
  error: {
    name: string;
    message: string;
    stack?: string;
    cause?: any;
  };
  deviceInfo: {
    userAgent: string;
    platform: string;
    language: string;
    cookieEnabled: boolean;
    onLine: boolean;
    screenWidth: number;
    screenHeight: number;
    windowWidth: number;
    windowHeight: number;
    pixelRatio: number;
  };
  iosInfo?: {
    standalone: boolean;
    maxTouchPoints: number;
    visualViewport?: {
      width: number;
      height: number;
      scale: number;
    };
    safariVersion?: string;
    isWebView?: boolean;
  };
  memoryInfo?: {
    usedJSHeapSize: number;
    totalJSHeapSize: number;
    jsHeapSizeLimit: number;
  };
  gameState?: {
    gameId?: string;
    route: string;
    storage: Record<string, string | null>;
    contexts?: Record<string, any>;
  };
  networkInfo?: {
    connectionType?: string;
    downlink?: number;
    effectiveType?: string;
    rtt?: number;
  };
  performanceInfo?: {
    navigation?: PerformanceNavigationTiming;
    paintTimings?: PerformanceEntry[];
  };
}

class ErrorLogger {
  private maxStoredErrors = 20;
  private storageKey = 'foolish_error_logs';
  private isIOS = /iPad|iPhone|iPod/.test(navigator.userAgent);
  private isSafari = /^((?!chrome|android).)*safari/i.test(navigator.userAgent);

  constructor() {
    this.setupGlobalErrorHandlers();
    this.setupIOSSpecificHandlers();
  }

  private setupGlobalErrorHandlers() {
    // Handle unhandled JavaScript errors
    window.addEventListener('error', (event) => {
      this.logError({
        type: 'javascript_error',
        context: 'Global Error Handler',
        error: {
          name: event.error?.name || 'Error',
          message: event.message,
          stack: event.error?.stack,
        },
        additionalInfo: {
          filename: event.filename,
          lineno: event.lineno,
          colno: event.colno,
        },
      });
    });

    // Handle unhandled promise rejections
    window.addEventListener('unhandledrejection', (event) => {
      this.logError({
        type: 'unhandled_rejection',
        context: 'Unhandled Promise Rejection',
        error: {
          name: event.reason?.name || 'UnhandledRejection',
          message: event.reason?.message || String(event.reason),
          stack: event.reason?.stack,
        },
      });
    });

    // Handle network errors (fetch failures)
    const originalFetch = window.fetch;
    window.fetch = async (...args) => {
      try {
        const response = await originalFetch(...args);
        if (!response.ok) {
          this.logError({
            type: 'network_error',
            context: 'Fetch Error',
            error: {
              name: 'FetchError',
              message: `HTTP ${response.status}: ${response.statusText}`,
            },
            additionalInfo: {
              url: args[0],
              status: response.status,
              statusText: response.statusText,
            },
          });
        }
        return response;
              } catch (error) {
          const err = error as Error;
          this.logError({
            type: 'network_error',
            context: 'Fetch Exception',
            error: {
              name: err.name || 'NetworkError',
              message: err.message || 'Fetch failed',
              stack: err.stack,
            },
          additionalInfo: {
            url: args[0],
          },
        });
        throw error;
      }
    };
  }

  private setupIOSSpecificHandlers() {
    if (!this.isIOS) return;

    // Monitor memory usage continuously
    this.startMemoryMonitoring();

    // Handle memory warnings on iOS (Safari specific)
    window.addEventListener('pagehide', () => {
      this.logError({
        type: 'ios_specific',
        context: 'Page Hide Event',
        error: {
          name: 'PageHide',
          message: 'Page is being hidden (potential memory warning)',
        },
        additionalInfo: {
          memoryInfo: this.getMemoryInfo(),
          performanceInfo: this.getPerformanceInfo(),
        },
      });
    });

    // Detect page freeze/unresponsive state
    this.setupFreezeDetection();

    // Monitor for excessive DOM mutations
    this.setupDOMObserver();

    // Monitor for excessive animations
    this.setupAnimationMonitoring();

    // Handle viewport changes that might indicate problems
    if (window.visualViewport) {
      let lastViewportHeight = window.visualViewport.height;
      window.visualViewport.addEventListener('resize', () => {
        const currentHeight = window.visualViewport!.height;
        const heightDiff = Math.abs(currentHeight - lastViewportHeight);
        
        // Log significant viewport changes that might indicate crashes
        if (heightDiff > 100) {
          this.logError({
            type: 'ios_specific',
            context: 'Visual Viewport Change',
            error: {
              name: 'ViewportResize',
              message: `Significant viewport height change: ${lastViewportHeight} -> ${currentHeight}`,
            },
            additionalInfo: {
              oldHeight: lastViewportHeight,
              newHeight: currentHeight,
              scale: window.visualViewport!.scale,
              width: window.visualViewport!.width,
            },
          });
        }
        lastViewportHeight = currentHeight;
      });
    }

    // Handle orientation changes
    window.addEventListener('orientationchange', () => {
      // Delay to get accurate measurements after orientation change
      setTimeout(() => {
        this.logError({
          type: 'ios_specific',
          context: 'Orientation Change',
          error: {
            name: 'OrientationChange',
            message: `Orientation changed to ${(window.screen as any).orientation?.angle || 'unknown'}`,
          },
          additionalInfo: {
            orientation: (window.screen as any).orientation?.angle,
            screenWidth: window.screen.width,
            screenHeight: window.screen.height,
            windowWidth: window.innerWidth,
            windowHeight: window.innerHeight,
          },
        });
      }, 100);
    });

    // Handle visibility changes (app going to background)
    document.addEventListener('visibilitychange', () => {
      if (document.hidden) {
        this.logError({
          type: 'ios_specific',
          context: 'Page Hidden',
          error: {
            name: 'VisibilityChange',
            message: 'Page became hidden (app backgrounded)',
          },
        });
      }
    });
  }

  private getDeviceInfo() {
    return {
      userAgent: navigator.userAgent,
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
  }

  private getIOSInfo() {
    if (!this.isIOS) return undefined;

    return {
      standalone: (navigator as any).standalone || false,
      maxTouchPoints: navigator.maxTouchPoints,
      visualViewport: window.visualViewport ? {
        width: window.visualViewport.width,
        height: window.visualViewport.height,
        scale: window.visualViewport.scale,
      } : undefined,
      safariVersion: this.getSafariVersion(),
      isWebView: !(document as any).webkitHidden && !(navigator as any).standalone,
    };
  }

  private getSafariVersion(): string | undefined {
    const match = navigator.userAgent.match(/Version\/(\d+\.\d+)/);
    return match ? match[1] : undefined;
  }

  private getMemoryInfo() {
    return (performance as any).memory ? {
      usedJSHeapSize: (performance as any).memory.usedJSHeapSize,
      totalJSHeapSize: (performance as any).memory.totalJSHeapSize,
      jsHeapSizeLimit: (performance as any).memory.jsHeapSizeLimit,
    } : undefined;
  }

  private getNetworkInfo() {
    const connection = (navigator as any).connection || (navigator as any).mozConnection || (navigator as any).webkitConnection;
    return connection ? {
      connectionType: connection.type,
      downlink: connection.downlink,
      effectiveType: connection.effectiveType,
      rtt: connection.rtt,
    } : undefined;
  }

  private getPerformanceInfo() {
    const navigation = performance.getEntriesByType('navigation')[0] as PerformanceNavigationTiming;
    const paintTimings = performance.getEntriesByType('paint');
    
    return {
      navigation,
      paintTimings,
    };
  }

  private getGameState() {
    try {
      return {
        gameId: window.location.pathname.split('/').pop(),
        route: window.location.pathname,
        storage: Object.keys(localStorage).reduce((acc, key) => {
          // Only include non-sensitive data
          if (!key.includes('password') && !key.includes('token') && !key.includes('auth')) {
            try {
              acc[key] = localStorage.getItem(key);
            } catch (e) {
              acc[key] = 'Error reading value';
            }
          }
          return acc;
        }, {} as Record<string, string | null>),
      };
    } catch (e) {
      return {
        gameId: 'unknown',
        route: window.location.pathname,
        storage: { error: 'Could not read localStorage' },
      };
    }
  }

  public logError({
    type,
    context,
    error,
    additionalInfo,
    userId,
  }: {
    type: ErrorLogEntry['type'];
    context: string;
    error: { name: string; message: string; stack?: string; cause?: any };
    additionalInfo?: any;
    userId?: string;
  }) {
    const errorId = `err_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`;
    const timestamp = new Date().toISOString();

    const logEntry: ErrorLogEntry = {
      errorId,
      timestamp,
      type,
      context,
      userId: userId || localStorage.getItem('user_id') || undefined,
      url: window.location.href,
      error,
      deviceInfo: this.getDeviceInfo(),
      iosInfo: this.getIOSInfo(),
      memoryInfo: this.getMemoryInfo(),
      gameState: this.getGameState(),
      networkInfo: this.getNetworkInfo(),
      performanceInfo: this.getPerformanceInfo(),
    };

    // Add additional info if provided
    if (additionalInfo) {
      (logEntry as any).additionalInfo = additionalInfo;
    }

    // Console log for immediate debugging
    const logLevel = type === 'javascript_error' || type === 'unhandled_rejection' ? 'error' : 'warn';
    console[logLevel](`🚨 [${type.toUpperCase()}] ${context}:`, logEntry);

    // Store in localStorage
    try {
      const storedErrors = JSON.parse(localStorage.getItem(this.storageKey) || '[]');
      storedErrors.push(logEntry);
      
      // Keep only the most recent errors
      if (storedErrors.length > this.maxStoredErrors) {
        storedErrors.splice(0, storedErrors.length - this.maxStoredErrors);
      }
      
      localStorage.setItem(this.storageKey, JSON.stringify(storedErrors));
    } catch (e) {
      console.error('Failed to store error log:', e);
    }

    // TODO: Send to external logging service if configured
    // this.sendToExternalService(logEntry);
  }

  public getStoredErrors(): ErrorLogEntry[] {
    try {
      return JSON.parse(localStorage.getItem(this.storageKey) || '[]');
    } catch (e) {
      console.error('Failed to retrieve stored errors:', e);
      return [];
    }
  }

  public clearStoredErrors() {
    localStorage.removeItem(this.storageKey);
  }

  public exportErrors(): string {
    const errors = this.getStoredErrors();
    return JSON.stringify(errors, null, 2);
  }

  // Manual error logging method for custom errors
  public logCustomError(context: string, error: Error, additionalInfo?: any) {
    this.logError({
      type: 'custom',
      context,
      error: {
        name: error.name,
        message: error.message,
        stack: error.stack,
      },
      additionalInfo,
    });
  }

  // Log performance issues
  public logPerformanceIssue(context: string, details: any) {
    this.logError({
      type: 'custom',
      context: `Performance Issue: ${context}`,
      error: {
        name: 'PerformanceIssue',
        message: 'Performance degradation detected',
      },
      additionalInfo: details,
    });
  }

  // Method to send errors to external service (implement as needed)
  private async sendToExternalService(logEntry: ErrorLogEntry) {
    // TODO: Implement sending to your backend or external logging service
    // Example:
    // try {
    //   await fetch('/api/log-error', {
    //     method: 'POST',
    //     headers: { 'Content-Type': 'application/json' },
    //     body: JSON.stringify(logEntry),
    //   });
    // } catch (e) {
    //   console.error('Failed to send error to external service:', e);
    // }
  }

  // Continuous memory monitoring for iOS
  private startMemoryMonitoring() {
    const checkMemory = () => {
      const memory = this.getMemoryInfo();
      if (memory) {
        const usagePercent = (memory.usedJSHeapSize / memory.jsHeapSizeLimit) * 100;
        
        if (usagePercent > 80) {
          this.logError({
            type: 'ios_specific',
            context: 'High Memory Usage',
            error: {
              name: 'MemoryWarning',
              message: `Memory usage at ${usagePercent.toFixed(1)}%`,
            },
            additionalInfo: {
              memoryInfo: memory,
              usagePercent,
            },
          });
        }
      }
    };

    // Check memory every 30 seconds
    setInterval(checkMemory, 30000);
    
    // Also check after significant actions
    document.addEventListener('visibilitychange', checkMemory);
    window.addEventListener('focus', checkMemory);
  }

  // Detect page freezes/unresponsive state
  private setupFreezeDetection() {
    let lastTime = Date.now();
    let freezeCount = 0;

    const checkFreeze = () => {
      const now = Date.now();
      const timeDiff = now - lastTime;
      
      // If more than 5 seconds have passed, consider it a freeze
      if (timeDiff > 5000) {
        freezeCount++;
        this.logError({
          type: 'ios_specific',
          context: 'Page Freeze Detected',
          error: {
            name: 'PageFreeze',
            message: `Page was unresponsive for ${(timeDiff / 1000).toFixed(1)}s`,
          },
          additionalInfo: {
            freezeDuration: timeDiff,
            freezeCount,
            memoryInfo: this.getMemoryInfo(),
          },
        });
      }
      
      lastTime = now;
    };

    // Check every 2 seconds
    setInterval(checkFreeze, 2000);
  }

  // Monitor excessive DOM mutations
  private setupDOMObserver() {
    if (!window.MutationObserver) return;

    let mutationCount = 0;
    let lastReset = Date.now();

    const observer = new MutationObserver((mutations) => {
      mutationCount += mutations.length;
      
      const now = Date.now();
      if (now - lastReset > 10000) { // Every 10 seconds
        if (mutationCount > 1000) {
          this.logError({
            type: 'ios_specific',
            context: 'Excessive DOM Mutations',
            error: {
              name: 'DOMThrashing',
              message: `${mutationCount} DOM mutations in 10 seconds`,
            },
            additionalInfo: {
              mutationsPerSecond: mutationCount / 10,
              memoryInfo: this.getMemoryInfo(),
            },
          });
        }
        mutationCount = 0;
        lastReset = now;
      }
    });

    observer.observe(document.body, {
      childList: true,
      subtree: true,
      attributes: true,
    });
  }

  // Monitor excessive animations
  private setupAnimationMonitoring() {
    let frameCount = 0;
    let lastCheck = Date.now();

    const countFrames = () => {
      frameCount++;
      const now = Date.now();
      
      if (now - lastCheck > 5000) { // Every 5 seconds
        const fps = (frameCount / 5);
        
        if (fps > 120) { // Excessive frame rate might indicate runaway animations
          this.logError({
            type: 'ios_specific',
            context: 'High Frame Rate',
            error: {
              name: 'ExcessiveAnimations',
              message: `${fps.toFixed(1)} FPS detected`,
            },
            additionalInfo: {
              fps,
              frameCount,
              memoryInfo: this.getMemoryInfo(),
            },
          });
        }
        
        frameCount = 0;
        lastCheck = now;
      }
      
      requestAnimationFrame(countFrames);
    };

    requestAnimationFrame(countFrames);
  }

  // Emergency diagnostic method
  public emergencyDiagnostic() {
    this.logError({
      type: 'custom',
      context: 'Emergency Diagnostic',
      error: {
        name: 'DiagnosticSnapshot',
        message: 'Manual diagnostic snapshot triggered',
      },
      additionalInfo: {
        timestamp: Date.now(),
        memoryInfo: this.getMemoryInfo(),
        performanceInfo: this.getPerformanceInfo(),
        domInfo: {
          elementsCount: document.querySelectorAll('*').length,
          bodyHTML: document.body.innerHTML.length,
        },
        activeElements: {
          activeElement: document.activeElement?.tagName,
          focusedElement: document.querySelector(':focus')?.tagName,
        },
      },
    });
  }
}

// Create and export singleton instance
export const errorLogger = new ErrorLogger();

// Export for manual use
export { ErrorLogger };