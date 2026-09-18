import { createContext, useContext, useState, ReactNode, useEffect } from 'react';
import { StringId, StringTable, Language, EN, LANGUAGES, isLanguage, loadLanguage, translate } from '../localization/strings';

export type { Language };

interface LocalizationContextType {
    language: Language;
    setLanguage: (lang: Language) => void;
    t: (stringId: StringId, params?: Record<string, string>) => string;
    /** Every language c/i18n declares, each naming itself. */
    languages: typeof LANGUAGES;
}

const LocalizationContext = createContext<LocalizationContextType | undefined>(undefined);

const LANGUAGE_STORAGE_KEY = 'foolish_language';

export const LocalizationProvider = ({ children }: { children: ReactNode }) => {
    // Default to English for the initial (prerendered) render so the static
    // shell can be generated without touching localStorage; the persisted
    // choice is loaded on mount in the browser below.
    const [language, setLanguageState] = useState<Language>('en');

    // The active language's table. English is here synchronously - it is the
    // fallback floor and the prerendered shell's language - and any other
    // language arrives from its own chunk a moment later, so the first paint is
    // never blank and never blocked on a fetch.
    const [table, setTable] = useState<StringTable>(EN);

    // Hydrate the persisted language from localStorage once, on the client.
    useEffect(() => {
        const stored = localStorage.getItem(LANGUAGE_STORAGE_KEY);
        if (stored && isLanguage(stored)) setLanguageState(stored);
    }, []);

    // Fetch whatever language is active. `live` guards the race: a player who
    // taps through three languages faster than the chunks arrive must end on
    // the one they last chose, not on whichever request happened to land last.
    useEffect(() => {
        let live = true;
        if (language === 'en') { setTable(EN); return; }
        loadLanguage(language).then((t) => { if (live) setTable(t); });
        return () => { live = false; };
    }, [language]);

    const setLanguage = (lang: Language) => {
        setLanguageState(lang);
        localStorage.setItem(LANGUAGE_STORAGE_KEY, lang);
    };

    const t = (stringId: StringId, params?: Record<string, string>): string =>
        translate(table, stringId, params);

    return (
        <LocalizationContext.Provider value={{ language, setLanguage, t, languages: LANGUAGES }}>
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
