import { useState } from 'react';
import { useAuth } from '../contexts/AuthContext';
import { useNavigate } from 'react-router-dom';

export const Login = () => {
  const [name, setName] = useState('');
  const [password, setPassword] = useState('');
  const navigate = useNavigate();

  const { signIn, signUp } = useAuth();

  const handleLogin = async (e: React.FormEvent) => {
    e.preventDefault();
    try {
      const { weakPassword } = await signIn(name, password);
      if (weakPassword) {
        alert('Weak password');
      }
      // No need to navigate manually - UnprotectedRoute will handle redirect
    } catch (error: any) {
      alert(error.message);
    }
  };

  const handleSignUp = async (e: React.FormEvent) => {
    e.preventDefault();
    try {
      await signUp(name, password);
      // not sure about this
      // navigate('/tutorial');
    } catch (error: any) {
      alert(error.message);
    }
  };

  return (
    <div>
      <form onSubmit={handleLogin}>
        <div>
          <label htmlFor="username">Username:</label>
          <input 
            id="username"
            type="text" 
            value={name} 
            onChange={(e) => setName(e.target.value)} 
            autoComplete="username"
            required
          />
        </div>
        <div>
          <label htmlFor="password">Password:</label>
          <input 
            id="password"
            type="password" 
            value={password} 
            onChange={(e) => setPassword(e.target.value)} 
            autoComplete="current-password"
            required
          />
        </div>
        <button type="submit">
          Login
        </button>
        <button type="button" onClick={handleSignUp}>
          Sign Up
        </button>
      </form>
    </div>
  );
};