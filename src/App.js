import React, { Suspense, lazy } from 'react';
import './App.css';
import { AuthProvider } from './contexts/AuthContext';
import { BrowserRouter, Routes, Route, Navigate } from 'react-router-dom';
import { ErrorBoundary } from './components/ErrorBoundary';
import { errorLogger } from './utils/errorLogger';

// Lazy load heavy components for better performance
const FernFractalProvider = lazy(() => 
  import('./utils/fernFractal').then(m => ({ default: m.FernFractalProvider }))
);
const Welcome = lazy(() => 
  import('./components/Welcome').then(m => ({ default: m.Welcome }))
);
const Login = lazy(() => 
  import('./components/Login').then(m => ({ default: m.Login }))
);
const Tutorial = lazy(() => 
  import('./components/Tutorial').then(m => ({ default: m.Tutorial }))
);
const Dashboard = lazy(() => 
  import('./components/Dashboard').then(m => ({ default: m.Dashboard }))
);
const GameView = lazy(() => 
  import('./components/GameView').then(m => ({ default: m.GameView }))
);
const ProtectedRoute = lazy(() => 
  import('./components/ProtectedRoute').then(m => ({ default: m.ProtectedRoute }))
);
const UnprotectedRoute = lazy(() => 
  import('./components/UnprotectedRoute').then(m => ({ default: m.UnprotectedRoute }))
);
const DebugPanel = lazy(() => 
  import('./components/DebugPanel').then(m => ({ default: m.DebugPanel }))
);
const WoolBackground = lazy(() => 
  import('./components/WoolBackground')
);
// SERVICE WORKERS ARE MAKING THIS EVEN MORE CONVOLUTED TO DEBUG
function App() {
  // Initialize error logging on app start
  React.useEffect(() => {
    console.log('🛡️ Error logging initialized');
    errorLogger.logCustomError('App Initialization', new Error('App started'), {
      timestamp: new Date().toISOString(),
      userAgent: navigator.userAgent,
      isIOS: /iPad|iPhone|iPod/.test(navigator.userAgent),
      memoryInfo: performance.memory ? {
        usedJSHeapSize: performance.memory.usedJSHeapSize,
        totalJSHeapSize: performance.memory.totalJSHeapSize,
        jsHeapSizeLimit: performance.memory.jsHeapSizeLimit,
      } : null,
    });

    // Add periodic crash detection check
    const crashCheckInterval = setInterval(() => {
      try {
        // Simple heartbeat to detect if the app is still responsive
        console.log('🔄 App heartbeat:', new Date().toISOString());
      } catch (error) {
        errorLogger.logCustomError('App Heartbeat Error', error);
      }
    }, 10000); // Every 10 seconds

    return () => clearInterval(crashCheckInterval);
  }, []);

  return (
    <ErrorBoundary context="App Root">
      <Suspense fallback={
        <div style={{ 
          display: 'flex', 
          alignItems: 'center', 
          justifyContent: 'center', 
          height: '100vh', 
          width: '100vw',
          backgroundColor: '#1a1a1a',
          color: '#fff',
          fontFamily: 'sans-serif'
        }}>
          Loading...
        </div>
      }>
        <WoolBackground />
        <div style={{ display: 'flex', height: '100vh', width: '100vw' }} >
          <DebugPanel />
          <BrowserRouter>
            <ErrorBoundary context="Router">
              <AuthProvider>
                <ErrorBoundary context="Auth Provider">
                  <Suspense fallback={<div style={{ display: 'flex', alignItems: 'center', justifyContent: 'center', height: '100vh', width: '100vw' }}>Loading app...</div>}>
                    <FernFractalProvider>
                      <ErrorBoundary context="Routes">
                        <Suspense fallback={<div style={{ display: 'flex', alignItems: 'center', justifyContent: 'center', height: '100vh', width: '100vw', color: '#fff' }}>Loading page...</div>}>
                          <Routes>
                            <Route path="/" element={
                              <ErrorBoundary context="Welcome Page">
                                <Welcome />
                              </ErrorBoundary>
                            } />
                            <Route path="/login" element={
                              <ErrorBoundary context="Login Page">
                                <UnprotectedRoute>
                                  <Login />
                                </UnprotectedRoute>
                              </ErrorBoundary>
                            } />
                            <Route path="/tutorial" element={
                              <ErrorBoundary context="Tutorial Page">
                                <Tutorial />
                              </ErrorBoundary>
                            } />

                            <Route path="/dashboard" element={
                              <ErrorBoundary context="Dashboard Page">
                                <ProtectedRoute>
                                  <Dashboard />
                                </ProtectedRoute>
                              </ErrorBoundary>
                            } />
                            <Route path="/:game_id" element={
                              <ErrorBoundary context="Game Page">
                                <ProtectedRoute>
                                  <GameView />
                                </ProtectedRoute>
                              </ErrorBoundary>
                            } />
                            {/* Catch-all route for unmatched paths - redirect to dashboard */}
                            <Route path="*" element={<Navigate to="/dashboard" replace />} />

                          </Routes>
                        </Suspense>
                      </ErrorBoundary>
                    </FernFractalProvider>
                  </Suspense>
                </ErrorBoundary>
              </AuthProvider>
            </ErrorBoundary>
          </BrowserRouter>
        </div>
      </Suspense>
    </ErrorBoundary>
  );
}

export default App;
