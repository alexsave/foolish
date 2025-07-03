import React, { createContext, useContext, useEffect, useState } from 'react';
import supabase from '../backend/Connector';
import { WEBSITE_DOMAIN } from '../constants/constants';
import { Session, User } from '@supabase/supabase-js';
import { WeakPassword } from '@supabase/supabase-js';
import { emailToName } from '../common/common_utils';

interface AuthContextType {
  user: string | null;
  user_id: string | null;
  signIn: (username: string, password: string) => Promise<{ user: User; session: Session; weakPassword?: WeakPassword; }>;
  signUp: (username: string, password: string) => Promise<{ user: User | null; session: Session | null; }>;
  signOut: () => Promise<void>;
  updatePassword: (newPassword: string) => Promise<void>;
  loading: boolean;
  redirectAfterLogin: string | null;
  setRedirectAfterLogin: (url: string) => void;
  clearRedirectAfterLogin: () => void;
}

const AuthContext = createContext<AuthContextType|null>(null);

const nameToEmail = (name: string): string => {
  return name + '@' + WEBSITE_DOMAIN;
}

// for now we'll just use a fake auth impl
export const AuthProvider = ({ children }: { children: React.ReactNode }) => {
  const [user, setUser] = useState<string | null>(null);
  const [user_id, setUserId] = useState<string | null>(null);
  const [name, setName] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);
  const [redirectAfterLogin, setRedirectAfterLoginState] = useState<string | null>(null);

  // Keep track of sessions tate
  useEffect(() => {
    // Check active sessions and sets the user
    supabase.auth.getSession().then(({ data: { session } }) => {
      if (session?.user) {
        const name = emailToName(session.user.email!);
        setUser(name);
        // useful for something I think
        setUserId(session.user.id);
      } else {
      }
      setLoading(false);
    });

    // Listen for changes on auth state (sign in, sign out, etc.)
    const { data: { subscription } } = supabase.auth.onAuthStateChange((event, session) => {
      if (session?.user) {
        setUser(emailToName(session.user.email!));
        setUserId(session.user.id);
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
    const email = nameToEmail(username);
    
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
    const email = nameToEmail(username);
    
    // Proceed with signup - Supabase handles duplicate email prevention
    // add name as a metadata field
    const { data, error } = await supabase.auth.signUp({ email, password });
    
    if (error) {
      throw error;
    }
    
    return data;
  };

  const signOut = async () => {
    try {
      console.log('Sign out initiated');
      console.log('Calling supabase.auth.signOut()');
      const { error } = await supabase.auth.signOut();
      
      if (error) {
        console.error('Supabase signOut error:', error);
        
        // Handle "Auth session missing" error specifically
        if (error.message === 'Auth session missing!' || 
            error.message.includes('session')) {
          console.log('Session missing error detected - performing local sign out');
          // Force a local sign out despite the error
          setUser(null);
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
      user,
      user_id,
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

export const useAuth = () => {
  const context = useContext(AuthContext);
  if (!context) {
    throw new Error('useAuth must be used within a AuthProvider');
  }
  return context;
}; 