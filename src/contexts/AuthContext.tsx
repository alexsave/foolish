import React, { createContext, useContext, useEffect, useState, useRef } from 'react';
import supabase from '../backend/Connector';
import { WEBSITE_DOMAIN } from '../constants/constants';
import { Session, User } from '@supabase/supabase-js';
import { Sha256 } from '@aws-crypto/sha256-js';
import { WeakPassword } from '@supabase/supabase-js';

const AuthContext = createContext<AuthContextType | null>(null);

const nameToEmail = async (name: string): Promise<string> => {
  const buf = new TextEncoder().encode(name);
  let digestArray: Uint8Array;
  // TODO remove this along with the aws lib
  if (window.location.hostname.startsWith('10.0.0')) {
    const hash = new Sha256();
    hash.update(buf);
    digestArray = await hash.digest();
  } else {
    const digest = await crypto.subtle.digest('SHA-256', buf);
    digestArray = new Uint8Array(digest);
  }

  const hex = Array.from(digestArray)
    .map(b => b.toString(16).padStart(2, '0'))
    .join('')
    .slice(0, 16);

  return `${hex}@${WEBSITE_DOMAIN}`
}

export const AuthProvider = ({ children }: { children: React.ReactNode }) => {
  const [user_id, setUserId] = useState<string | null>(null);
  const [username, setUsername] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);
  const [redirectAfterLogin, setRedirectAfterLoginState] = useState<string | null>(null);

  // Use ref to track current user_id to prevent duplicate updates
  const currentUserIdRef = useRef<string | null>(null);

  // Keep track of sessions tate
  useEffect(() => {
    // Check active sessions and sets the user
    supabase.auth.getSession().then(({ data: { session } }) => {
      if (session?.user) {
        // Only update if user ID has actually changed
        if (currentUserIdRef.current !== session.user.id) {
          currentUserIdRef.current = session.user.id;
          //setUser(emailToName(session.user.email!));
          setUserId(session.user.id);
          setUsername(session.user.user_metadata.username);
        }
      }
      setLoading(false);
    });

    // Listen for changes on auth state (sign in, sign out, etc.)
    const { data: { subscription } } = supabase.auth.onAuthStateChange((event, session) => {
      if (session?.user) {
        if (currentUserIdRef.current !== session.user.id) {
          currentUserIdRef.current = session.user.id;
          //setUser(emailToName(session.user.email!));
          setUserId(session.user.id);
          setUsername(session.user.user_metadata.username);
        }
      } else {
        // Handle sign out case
        if (currentUserIdRef.current !== null) {
          currentUserIdRef.current = null;
          setUserId(null);
          setUsername(null);
        }
      }
    });

    return () => subscription.unsubscribe();
  }, []);

  const setRedirectAfterLogin = (url: string) => {
    setRedirectAfterLoginState(url);
  };

  const clearRedirectAfterLogin = () => {
    setRedirectAfterLoginState(null);
  };

  const signIn = async (username: string, password: string) => {
    const email = await nameToEmail(username);

    const { data, error } = await supabase.auth.signInWithPassword({
      email,
      password,
    });

    if (error) {
      throw error;
    }

    return data;
  };

  const signUp = async (username: string, password: string) => {
    // this is a hack to get around the fact that supabase doesn't support username/password auth
    const email = await nameToEmail(username);

    // Proceed with signup - Supabase handles duplicate email prevention
    // add name as a metadata field
    const { data, error } = await supabase.auth.signUp({ email, password, options: { data: { username: username } } });

    if (error) {
      throw error;
    }

    return data;
  };

  const signOut = async () => {
    try {
      const { error } = await supabase.auth.signOut();

      if (error) {
        console.error('Supabase signOut error:', error);

        // Handle "Auth session missing" error specifically
        if (error.message === 'Auth session missing!' ||
          error.message.includes('session')) {
          console.log('Session missing error detected - performing local sign out');
          // Force a local sign out despite the error
          setUserId(null);
          setUsername(null);
          console.log('Local sign out completed');
          return; // Exit without throwing error since we've handled it
        }

        throw error;
      }
      console.log('Supabase sign out successful');
    } catch (error) {
      console.error('Sign out error:', error);
      throw error;
    }
  };

  const updatePassword = async (newPassword: string) => {
    const { error } = await supabase.auth.updateUser({
      password: newPassword,
    });
    if (error) throw error;
  };

  return (
    <AuthContext.Provider value={{
      user_id,
      username,
      loading,
      signIn,
      signUp,
      signOut,
      updatePassword,
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
  signIn: (username: string, password: string) => Promise<{ user: User; session: Session; weakPassword?: WeakPassword; }>;
  signUp: (username: string, password: string) => Promise<{ user: User | null; session: Session | null; }>;
  signOut: () => Promise<void>;
  updatePassword: (newPassword: string) => Promise<void>;
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