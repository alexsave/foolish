import { useLocalization, Language } from '../contexts/LocalizationContext';
import { useMemo } from 'react';

interface LanguageConfig {
  code: Language;
  flag: string;
  label: string;
}

const LANGUAGES: LanguageConfig[] = [
  { code: 'en', flag: '🇺🇸', label: 'EN' },
  { code: 'ru', flag: '🇷🇺', label: 'РУ' },
  { code: 'ko', flag: '🇰🇷', label: '한' },
];

export const LanguageSwitcher = () => {
  const { language, setLanguage } = useLocalization();

  const buttonStyle: React.CSSProperties = useMemo(() => ({
    border: 'none',
    borderRadius: '4px',
    fontWeight: 'bold',
    cursor: 'pointer',
    transition: 'all 0.2s ease',
    position: 'relative' as const,
    overflow: 'hidden' as const,
    padding: '0',
    fontSize: '18px',
    width: '40px',
    height: '40px',
    minWidth: '40px',
    minHeight: '40px',
    display: 'flex',
    alignItems: 'center',
    justifyContent: 'center',
    backgroundColor: 'transparent',
  }), []);

  const activeLanguageStyle: React.CSSProperties = useMemo(() => ({
    ...buttonStyle,
    opacity: 0.6,
    cursor: 'default',
  }), [buttonStyle]);

  const flagStyle: React.CSSProperties = {
    position: 'absolute',
    fontSize: '50px',
    top: '50%',
    left: '50%',
    transform: 'translate(-50%, -50%)',
    zIndex: 0,
  };

  const labelStyle: React.CSSProperties = {
    position: 'relative',
    zIndex: 1,
    color: 'white',
    textShadow: '0 0 3px black, 0 0 3px black, 0 0 3px black',
    fontWeight: 'bold',
  };

  return (
    <div style={{
      position: 'fixed',
      bottom: '10px',
      right: '10px',
      display: 'flex',
      gap: '8px',
      zIndex: 1000,
    }}>
      {LANGUAGES.map(({ code, flag, label }) => {
        const isActive = language === code;
        return (
          <button
            key={code}
            onClick={() => setLanguage(code)}
            disabled={isActive}
            style={isActive ? activeLanguageStyle : buttonStyle}
          >
            <span style={flagStyle}>{flag}</span>
            <span style={labelStyle}>{label}</span>
          </button>
        );
      })}
    </div>
  );
};

