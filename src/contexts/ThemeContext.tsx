import { createContext, useContext, useEffect, ReactNode } from 'react';
import { useLocalization, Language } from './LocalizationContext';

type Theme = 'default' | 'soviet';

interface ThemeContextType {
  theme: Theme;
  isSoviet: boolean;
}

const ThemeContext = createContext<ThemeContextType | undefined>(undefined);

// Map languages to themes
const LANGUAGE_THEMES: Record<Language, Theme> = {
  en: 'default',
  ru: 'soviet',
  ko: 'default',
};

export const ThemeProvider = ({ children }: { children: ReactNode }) => {
  const { language } = useLocalization();
  const theme = LANGUAGE_THEMES[language];
  const isSoviet = theme === 'soviet';

  // Apply theme to document root (html element) for CSS variable targeting
  // Must be on documentElement for [data-theme="soviet"] to override :root variables
  useEffect(() => {
    document.documentElement.setAttribute('data-theme', theme);
    document.body.setAttribute('data-theme', theme); // Also on body for component selectors
    
    // Also add/remove a class for easier CSS targeting
    if (isSoviet) {
      document.body.classList.add('theme-soviet');
      document.body.classList.remove('theme-default');
    } else {
      document.body.classList.add('theme-default');
      document.body.classList.remove('theme-soviet');
    }

    return () => {
      document.documentElement.removeAttribute('data-theme');
      document.body.removeAttribute('data-theme');
      document.body.classList.remove('theme-soviet', 'theme-default');
    };
  }, [theme, isSoviet]);

  return (
    <ThemeContext.Provider value={{ theme, isSoviet }}>
      {children}
    </ThemeContext.Provider>
  );
};

export const useTheme = () => {
  const context = useContext(ThemeContext);
  if (!context) {
    throw new Error('useTheme must be used within a ThemeProvider');
  }
  return context;
};
