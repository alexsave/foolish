import './App.css';
import { AuthProvider } from './contexts/AuthContext';
import { BrowserRouter, Routes, Route } from 'react-router-dom';
import { Welcome } from './components/Welcome';
import { Login } from './components/Login';
import { Tutorial } from './components/Tutorial';
import { ProtectedRoute } from './components/ProtectedRoute';
import { Dashboard } from './components/Dashboard';
import { Lobby } from './components/Lobby';
import { GameDisplay } from './components/GameDisplay';
import { UnprotectedRoute } from './components/UnprotectedRoute';


function App() {
  return (
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
              <Lobby />
            </ProtectedRoute>
          } />
          <Route path="/game/:game_id" element={
            <ProtectedRoute>
              <GameDisplay />
            </ProtectedRoute>
          } />

        </Routes>
      </AuthProvider>
    </BrowserRouter>
  );
}

export default App;
