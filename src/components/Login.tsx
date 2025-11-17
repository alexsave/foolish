import { useState } from 'react';
import { useAuth } from '../contexts/AuthContext';
import { useWoodStyle } from './WoodTexture';

export const Login = () => {
  const [name, setName] = useState('');
  const [password, setPassword] = useState('');

  const { signIn, signUp } = useAuth();
  
  const woodStyle1 = useWoodStyle(0.4);
  const woodStyle2 = useWoodStyle(0.9);

  const woodButtonStyle: React.CSSProperties = {
    ...woodStyle1, // Random position seed 0.4
    border: '3px solid #5D3A1A', // Darker wood border color
    borderRadius: '0', // Sharp 90-degree corners
    color: '#ffffff',
    fontWeight: 'bold',
    cursor: 'pointer',
    transition: 'all 0.2s ease',
    textShadow: '2px 2px 4px rgba(0,0,0,0.9)',
    boxShadow: `
        inset 0 1px 0 rgba(255,255,255,0.2),
        inset 0 -1px 0 rgba(0,0,0,0.3),
        0 2px 4px rgba(0,0,0,0.4)`,
    position: 'relative' as const,
    overflow: 'hidden' as const,
    padding: '10px 20px',
    fontSize: '16px',
    marginRight: '10px',
  };

  const woodButtonStyle2: React.CSSProperties = {
    ...woodStyle2, // Different random position seed 0.9
    border: '3px solid #5D3A1A', // Darker wood border color
    borderRadius: '0', // Sharp 90-degree corners
    color: '#ffffff',
    fontWeight: 'bold',
    cursor: 'pointer',
    transition: 'all 0.2s ease',
    textShadow: '2px 2px 4px rgba(0,0,0,0.9)',
    boxShadow: `
        inset 0 1px 0 rgba(255,255,255,0.2),
        inset 0 -1px 0 rgba(0,0,0,0.3),
        0 2px 4px rgba(0,0,0,0.4)`,
    position: 'relative' as const,
    overflow: 'hidden' as const,
    padding: '10px 20px',
    fontSize: '16px',
  };

  const woodButtonHoverStyle: React.CSSProperties = {
    ...woodStyle1, // Match first button seed
    filter: 'brightness(1.1) contrast(1.1)',
    transform: 'translateY(-1px)',
    boxShadow: `
        inset 0 2px 0 rgba(255,255,255,0.3),
        inset 0 -2px 0 rgba(0,0,0,0.4),
        0 4px 8px rgba(0,0,0,0.5)`,
  };

  const woodButtonHoverStyle2: React.CSSProperties = {
    ...woodStyle2, // Match second button seed
    filter: 'brightness(1.1) contrast(1.1)',
    transform: 'translateY(-1px)',
    boxShadow: `
        inset 0 2px 0 rgba(255,255,255,0.3),
        inset 0 -2px 0 rgba(0,0,0,0.4),
        0 4px 8px rgba(0,0,0,0.5)`,
  };

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
        <button 
          type="submit"
          style={woodButtonStyle}
          onMouseEnter={(e) => {
            Object.assign(e.currentTarget.style, woodButtonHoverStyle);
          }}
          onMouseLeave={(e) => {
            Object.assign(e.currentTarget.style, woodButtonStyle);
          }}
        >
          Login
        </button>
        <button 
          type="button" 
          onClick={handleSignUp}
          style={woodButtonStyle2}
          onMouseEnter={(e) => {
            Object.assign(e.currentTarget.style, woodButtonHoverStyle2);
          }}
          onMouseLeave={(e) => {
            Object.assign(e.currentTarget.style, woodButtonStyle2);
          }}
        >
          Sign Up
        </button>
      </form>
    </div>
  );
};