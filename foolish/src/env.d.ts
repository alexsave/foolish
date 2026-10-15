declare namespace NodeJS {
    interface ProcessEnv {
      /** c_base, the backend (its binary surface): blank for same-origin. */
      NEXT_PUBLIC_BACKEND_URL: string;
    }
  }
