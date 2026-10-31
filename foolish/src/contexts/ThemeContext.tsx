import { createContext, useContext, useEffect, ReactNode } from 'react';
import { useLocalization, Language } from './LocalizationContext';

type Theme = 'default' | 'soviet';

interface ThemeContextType {
  theme: Theme;
  isSoviet: boolean;
}

const ThemeContext = createContext<ThemeContextType | undefined>(undefined);

// THE SOVIET THEME IS NOT CHOSEN BY LANGUAGE ANY MORE. Russian used to switch it
// on (a language -> theme map here: ru -> 'soviet', en and ko -> 'default', and
// every other language fell through to no theme at all). Since 2026-10-31 every
// language reads the default theme. The theme itself is kept - its CSS
// ([data-theme="soviet"]) and the components that read isSoviet - and nothing
// selects it: there is no theme picker. Turning it back on for Russian is this
// one rule.
const themeFor = (_language: Language): Theme => 'default';

export const ThemeProvider = ({ children }: { children: ReactNode }) => {
  const { language } = useLocalization();
  const theme = themeFor(language);
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
