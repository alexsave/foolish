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
  // Initialize language from localStorage or default to English
  const [language, setLanguage] = useState<Language>(() => {
    const stored = localStorage.getItem(LANGUAGE_STORAGE_KEY);
    return (stored === 'en' || stored === 'ru' || stored === 'ko') ? stored : 'en';
  });

  // Persist language changes to localStorage
  useEffect(() => {
    localStorage.setItem(LANGUAGE_STORAGE_KEY, language);
  }, [language]);

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

