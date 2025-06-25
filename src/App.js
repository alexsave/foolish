import './App.css';
import { AuthProvider, useAuth } from './contexts/AuthContext';
import logo from './logo.svg';
import { BrowserRouter, Routes, Route } from 'react-router-dom';
import { GameDisplay } from './components/GameDisplay';

function App() {
  const { user } = useAuth();

  return (
    <BrowserRouter>
      <AuthProvider>
        <Routes>
          <Route path="/" element={
            <div>
              <p>Home</p>
            </div>
          } />
          <Route path="/login" element={
            user ? (
              <div>
                <p>Welcome, {user.email}</p>
              </div>
            ) : (
              <div>
                <p>Please sign in</p>

              </div>
            )
          } />
          <Route path="/game" element={
            <GameDisplay />
          } />
        </Routes>
      </AuthProvider>
    </BrowserRouter>
  );
}

export default App;
