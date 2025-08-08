import React from 'react';
import './App.css';
import { AuthProvider } from './contexts/AuthContext';
import { FernFractalProvider } from './utils/fernFractal';
import { BrowserRouter, Routes, Route, Navigate } from 'react-router-dom';
import { Welcome } from './components/Welcome';
import { Login } from './components/Login';
import { Tutorial } from './components/Tutorial';
import { ProtectedRoute } from './components/ProtectedRoute';
import { Dashboard } from './components/Dashboard';
import { GameView } from './components/GameView';
import { UnprotectedRoute } from './components/UnprotectedRoute';
import { ErrorBoundary } from './components/ErrorBoundary';
import { DebugPanel } from './components/DebugPanel';
import { errorLogger } from './utils/errorLogger';
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
      <div style={{ display: 'flex', height: '100vh', width: '100vw' }} >
        <DebugPanel />
        <BrowserRouter>
          <ErrorBoundary context="Router">
            <AuthProvider>
              <ErrorBoundary context="Auth Provider">
                <FernFractalProvider>
                  <ErrorBoundary context="Routes">
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
                  </ErrorBoundary>
                </FernFractalProvider>
              </ErrorBoundary>
            </AuthProvider>
          </ErrorBoundary>
        </BrowserRouter>
      </div>
    </ErrorBoundary>
  );
}

export default App;
