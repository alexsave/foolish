import { createContext, useContext, useState, ReactNode, useEffect } from 'react';
import { StringId, strings } from '../localization/strings';

export type Language = 'en' | 'ru' | 'ko';

interface LocalizationContextType {
  language: Language;
  setLanguage: (lang: Language) => void;
  t: (stringId: StringId, params?: Record<string, string>) => string;
}

const LocalizationContext = createContext<LocalizationContextType | undefined>(undefined);

const LANGUAGE_STORAGE_KEY = 'foolish_language';

export const LocalizationProvider = ({ children }: { children: ReactNode }) => {
  // Default to English for the initial (prerendered) render so the static shell
  // can be generated without touching localStorage; the persisted choice is
  // loaded on mount in the browser below.
  const [language, setLanguageState] = useState<Language>('en');

  // Hydrate the persisted language from localStorage once, on the client.
  useEffect(() => {
    const stored = localStorage.getItem(LANGUAGE_STORAGE_KEY);
    if (stored === 'en' || stored === 'ru' || stored === 'ko') {
      setLanguageState(stored);
    }
  }, []);

  // Update language and persist the choice.
  const setLanguage = (lang: Language) => {
    setLanguageState(lang);
    localStorage.setItem(LANGUAGE_STORAGE_KEY, lang);
  };

  // Translation function with parameter substitution
  const t = (stringId: StringId, params?: Record<string, string>): string => {
    let translation = strings[language][stringId];
    
    // Replace parameters in the format {key} with values
    if (params) {
      Object.entries(params).forEach(([key, value]) => {
        translation = translation.replace(`{${key}}`, value);
      });
    }
    
    return translation;
  };

  return (
    <LocalizationContext.Provider value={{ language, setLanguage, t }}>
      {children}
    </LocalizationContext.Provider>
  );
};

export const useLocalization = () => {
  const context = useContext(LocalizationContext);
  if (!context) {
    throw new Error('useLocalization must be used within a LocalizationProvider');
  }
  return context;
};

