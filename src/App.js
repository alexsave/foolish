import './App.css';
import { AuthProvider } from './contexts/AuthContext';
import { BrowserRouter, Routes, Route } from 'react-router-dom';
import { Welcome } from './components/Welcome';
import { ServerProvider } from './contexts/ServerContext';
import { Login } from './components/Login';
import { Tutorial } from './components/Tutorial';
import { ProtectedContent } from './components/ProtectedContent';

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
            <Route path="/*" element={
              <ProtectedContent/>
            } />

          </Routes>
        </ServerProvider>
      </AuthProvider>
    </BrowserRouter>
  );
}

export default App;
