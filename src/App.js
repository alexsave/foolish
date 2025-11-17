import React, { Suspense, lazy } from 'react';
import './App.css';
import { AuthProvider } from './contexts/AuthContext';
import { BrowserRouter, Routes, Route, Navigate } from 'react-router-dom';
import { ErrorBoundary } from './components/ErrorBoundary';
// Eagerly import critical infrastructure to reduce round trips
import { ProtectedRoute } from './components/ProtectedRoute';
import { UnprotectedRoute } from './components/UnprotectedRoute';

// Lazy load route components with preload hints for faster loading
const Welcome = lazy(() => 
  import(/* webpackPreload: true */ './components/Welcome').then(m => ({ default: m.Welcome }))
);
const Login = lazy(() => 
  import(/* webpackPreload: true */ './components/Login').then(m => ({ default: m.Login }))
);
const Tutorial = lazy(() => 
  import('./components/Tutorial').then(m => ({ default: m.Tutorial }))
);
const Dashboard = lazy(() => 
  import(/* webpackPrefetch: true */ './components/Dashboard').then(m => ({ default: m.Dashboard }))
);
const GameView = lazy(() => 
  import(/* webpackPreload: true */ './components/GameView').then(m => ({ default: m.GameView }))
);


// SERVICE WORKERS ARE MAKING THIS EVEN MORE CONVOLUTED TO DEBUG
function App() {
  // Initialize error logging on app start
  React.useEffect(() => {
    console.log('🛡️ Error logging initialized');
    // Add periodic crash detection check
    const crashCheckInterval = setInterval(() => {
      try {
        // Simple heartbeat to detect if the app is still responsive
        console.log('🔄 App heartbeat:', new Date().toISOString());
      } catch (error) {
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
          Loading App.js...
        </div>
      }>
        <div style={{ display: 'flex', height: '100vh', width: '100vw' }} >
          <BrowserRouter>
            <ErrorBoundary context="Router">
              <AuthProvider>
                <ErrorBoundary context="Auth Provider">
                  <Suspense fallback={<div style={{ display: 'flex', alignItems: 'center', justifyContent: 'center', height: '100vh', width: '100vw' }}>Loading app...</div>}>
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
