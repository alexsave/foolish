import React, { createContext, useContext, useEffect, useState } from 'react';
import { onUserChange, signedInUser } from '../common/session';

// Exported so self-contained screens (e.g. the tutorial) can re-provide a
// mock identity for a seat without a real session.
export const AuthContext = createContext<AuthContextType | null>(null);

export const AuthProvider = ({ children }: { children: React.ReactNode }) => {
  const [user_id, setUserId] = useState<string | null>(null);
  const [username, setUsername] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);
  const [redirectAfterLogin, setRedirectAfterLoginState] = useState<string | null>(null);

  // Who is signed in, as this browser remembers it (src/common/session.ts),
  // and again whenever that changes, here or in another tab. Signing in and
  // out is backend.ts's (the kernel's client), on the pages that do it.
  useEffect(() => {
    const apply = () => {
      const u = signedInUser();
      setUserId(u?.id ?? null);
      setUsername(u?.username ?? null);
    };
    apply();
    setLoading(false);
    return onUserChange(apply);
  }, []);

  const setRedirectAfterLogin = (url: string) => setRedirectAfterLoginState(url);
  const clearRedirectAfterLogin = () => setRedirectAfterLoginState(null);

  return (
    <AuthContext.Provider value={{
      user_id,
      username,
      loading,
      redirectAfterLogin,
      setRedirectAfterLogin,
      clearRedirectAfterLogin,
    }}>
      {children}
    </AuthContext.Provider>
  );
};

interface AuthContextType {
  user_id: string | null;
  username: string | null;
  loading: boolean;
  redirectAfterLogin: string | null;
  setRedirectAfterLogin: (url: string) => void;
  clearRedirectAfterLogin: () => void;
}


export const useAuth = () => {
  const context = useContext(AuthContext);
  if (!context) {
    throw new Error('useAuth must be used within a AuthProvider');
  }
  return context;
}; 