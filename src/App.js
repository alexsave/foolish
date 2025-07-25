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
import WoolBackground from './components/WoolBackground';

function App() {
  return (
    <div style={{ display: 'flex', height: '100vh', width: '100vw' }} >
      <WoolBackground/>
      <BrowserRouter>
        <AuthProvider>
          <FernFractalProvider>
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
                <GameView />
              </ProtectedRoute>
            } />
            {/* Catch-all route for unmatched paths - redirect to dashboard */}
            <Route path="*" element={<Navigate to="/dashboard" replace />} />

            </Routes>
          </FernFractalProvider>
        </AuthProvider>
      </BrowserRouter>
    </div>
  );
}

export default App;
