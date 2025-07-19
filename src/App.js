import './App.css';
import { AuthProvider } from './contexts/AuthContext';
import { BrowserRouter, Routes, Route, Navigate } from 'react-router-dom';
import { Welcome } from './components/Welcome';
import { Login } from './components/Login';
import { Tutorial } from './components/Tutorial';
import { ProtectedRoute } from './components/ProtectedRoute';
import { Dashboard } from './components/Dashboard';
import { GameView } from './components/GameView';
import { UnprotectedRoute } from './components/UnprotectedRoute';
import { GameProvider } from './contexts/GameContext';
import { DragProvider } from './contexts/DragContext';
import { AnimationProvider } from './contexts/AnimationContext';

function App() {
  return (
    <div style={{ backgroundColor: '#982621', display: 'flex', height: '100vh', width: '100vw' }} >
      <BrowserRouter>
        <AuthProvider>
          <Routes>
            <Route path="/" element={
              <Welcome />
            } />
            <Route path="/login" element={
              <UnprotectedRoute>
                <Login />
              </UnprotectedRoute>
            } />
            <Route path="/tutorial" element={
              <Tutorial />
            } />

            <Route path="/dashboard" element={
              <ProtectedRoute>
                <Dashboard />
              </ProtectedRoute>
            } />
            <Route path="/:game_id" element={
              <ProtectedRoute>
                <AnimationProvider>
                  <GameProvider>
                    <DragProvider>
                      <GameView />
                    </DragProvider>
                  </GameProvider>
                </AnimationProvider>
              </ProtectedRoute>
            } />
            {/* Catch-all route for unmatched paths - redirect to dashboard */}
            <Route path="*" element={<Navigate to="/dashboard" replace />} />

          </Routes>
        </AuthProvider>
      </BrowserRouter>
    </div>
  );
}

export default App;
