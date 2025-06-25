import React, { createContext, useContext, useEffect, useState } from 'react';
//import supabase from '../db/supabaseClient';

const AuthContext = createContext({});

// for now we'll just use a fake auth impl
export const AuthProvider = ({ children }) => {
  const [user, setUser] = useState(null);
  const [loading, setLoading] = useState(true);

  useEffect(() => {
  }, []);


  return (
    <AuthContext.Provider value={{
      user
    }}>
      {children}
    </AuthContext.Provider>
  );
};

export const useAuth = () => {
  return useContext(AuthContext);
}; 