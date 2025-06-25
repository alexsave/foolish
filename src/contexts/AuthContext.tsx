import React, { createContext, useContext, useEffect, useState } from 'react';
//import supabase from '../db/supabaseClient';
import { useServer } from './ServerContext';

interface AuthContextType {
  user: {name: string} | null;
  login: (name: string) => void;
}

const AuthContext = createContext<AuthContextType|null>(null);

// for now we'll just use a fake auth impl
export const AuthProvider = ({ children }: { children: React.ReactNode }) => {
  const [user, setUser] = useState<{name: string} | null>(null);
  const [name, setName] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);

  useEffect(() => {
  }, []);


  const {serverLogin} = useServer();

  const login = (name: string) => {
    setUser({name});
    serverLogin(name).then(data => {
      setName(data.name);
    });
  };

  return (
    <AuthContext.Provider value={{
      user,
      login
    }}>
      {children}
    </AuthContext.Provider>
  );
};

export const useAuth = () => {
  const context = useContext(AuthContext);
  if (!context) {
    throw new Error('useAuth must be used within a AuthProvider');
  }
  return context;
}; 