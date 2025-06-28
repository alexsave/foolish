import './App.css';
import { AuthProvider, useAuth } from './contexts/AuthContext';
import logo from './logo.svg';
import { BrowserRouter, Routes, Route, useNavigate } from 'react-router-dom';
import { GameDisplay } from './components/GameDisplay';
import { Welcome } from './components/Welcome';
import { ServerProvider } from './contexts/ServerContext';
import { Login } from './components/Login';
import { Dashboard } from './components/Dashboard';
import { Lobby } from './components/Lobby';
import { Tutorial } from './components/Tutorial';

function App() {

  return (
    <BrowserRouter>
      <AuthProvider>
        <ServerProvider>

          <Routes>
            <Route path="/" element={
              <Welcome />
            } />
            <Route path="/login" element={
              <Login />
            } />
            <Route path="/tutorial" element={
              <Tutorial />
            } />
            <Route path="/dashboard" element={
              <Dashboard />
            } />
            <Route path="/:game_id" element={
              <Lobby />
            } />
            <Route path="/game/:game_id" element={
              <GameDisplay />
            } />
          </Routes>
        </ServerProvider>
      </AuthProvider>
    </BrowserRouter>
  );
}

export default App;
