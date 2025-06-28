declare namespace NodeJS {
    interface ProcessEnv {
      //NODE_ENV: 'development' | 'production' | 'test';
      //PUBLIC_URL: string;
      //REACT_APP_API_URL: string;
      //REACT_APP_ANALYTICS_ID: string;
      REACT_APP_SUPABASE_URL: string;
      REACT_APP_SUPABASE_KEY: string;
    }
  }