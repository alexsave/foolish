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

function App() {

  return (
    <BrowserRouter>
      <ServerProvider>

        <AuthProvider>
          <Routes>
            <Route path="/" element={
              <Welcome />
            } />
            <Route path="/login" element={
              <Login />
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
        </AuthProvider>
      </ServerProvider>
    </BrowserRouter>
  );
}

export default App;
