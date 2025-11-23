import React from 'react';
import './App.css';
import { AuthProvider } from './contexts/AuthContext';
import { BrowserRouter, Routes, Route, Navigate } from 'react-router-dom';
import { ErrorBoundary } from './components/ErrorBoundary';
import { ProtectedRoute } from './components/ProtectedRoute';
import { UnprotectedRoute } from './components/UnprotectedRoute';
import { Welcome } from './components/Welcome';
import { Tutorial } from './components/Tutorial';
import { Dashboard } from './components/Dashboard';
import { GameView } from './components/GameView';


// SERVICE WORKERS ARE MAKING THIS EVEN MORE CONVOLUTED TO DEBUG
function App() {
  return (
    <ErrorBoundary context="App Root">
      <div style={{ display: 'flex', height: '100vh', width: '100vw' }} >
        <BrowserRouter>
          <ErrorBoundary context="Router">
            <AuthProvider>
              <ErrorBoundary context="Auth Provider">
                <ErrorBoundary context="Routes">
                  <Routes>
                    <Route path="/" element={
                      <ErrorBoundary context="Welcome Page">
                        <UnprotectedRoute>
                          <Welcome />
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
              </ErrorBoundary>
            </AuthProvider>
          </ErrorBoundary>
        </BrowserRouter>
      </div>
    </ErrorBoundary>
  );
}

export default App;
