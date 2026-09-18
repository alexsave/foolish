// The languages the app carries, and everything about a language that is not a
// translation: its code, the name it calls itself, and whether it is written
// right to left.
//
// THE CODE IS THE MATCH. Each entry's code is the ISO 639-1 subtag a speaker's
// locale begins with, which is what lets the phone app resolve a language out of
// `Locale.preferredLanguages` without a hand-written mapping, and what the web
// stores in localStorage. The resolver itself stays in each host: iOS has four
// subtags that are not their own name (nb/nn -> no, in -> id, iw -> he) and the
// browser has its own habits.
//
// NO REGIONAL VARIANTS, deliberately. One `pt` serves Brazil and Portugal, one
// `zh` is Simplified and a Traditional reader lands somewhere they can read
// rather than in English.
#ifndef FOOLISH_I18N_LANGUAGES_H
#define FOOLISH_I18N_LANGUAGES_H

typedef enum {
    FS_L_EN,
    FS_L_RU,
    FS_L_KO,
    FS_L_ZH,
    FS_L_VI,
    FS_L_ES,
    FS_L_PT,
    FS_L_FR,
    FS_L_DE,
    FS_L_IT,
    FS_L_JA,
    FS_L_PL,
    FS_L_UK,
    FS_L_TR,
    FS_L_ID,
    FS_L_TH,
    FS_L_NL,
    FS_L_SV,
    FS_L_DA,
    FS_L_NO,
    FS_L_FI,
    FS_L_CS,
    FS_L_RO,
    FS_L_HE,
    FS_L_AR,
    FS_L_COUNT
} FsLang;

// One row per language file under c/i18n, and that is the registry: adding a
// language is adding its strings_<code>.c and its row here. tools/structgen/gen.sh
// READS THIS TABLE to find out what to generate, so no build script, no CI lane
// and no host keeps a list of languages that could fall behind this one.
//
// This is also where the questions a language raises that are NOT translations
// get answered - what it calls itself, and which way it is written. The website
// hard-codes those for three languages in LanguageSwitcher.tsx today while the
// phone app carries twenty-five of them in a Swift switch.
//
// It is a table of STRUCTS on purpose. datagen reads a row's fields by name the
// same way it reads an array's slots by designator, so a column added here
// reaches both hosts without either one being edited.
typedef struct {
    const char *code;     // the ISO 639-1 subtag a speaker's locale begins with
    const char *display;  // the name the language calls ITSELF, in its own script
    int         rtl;      // written right to left (it does not flip the board)
} FsLanguage;

static const FsLanguage FS_LANGUAGES[FS_L_COUNT] = {
    [FS_L_EN] = { "en", "English",          0 },
    [FS_L_RU] = { "ru", "Русский",          0 },
    [FS_L_KO] = { "ko", "한국어",              0 },
    [FS_L_ZH] = { "zh", "中文",               0 },
    [FS_L_VI] = { "vi", "Tiếng Việt",       0 },
    [FS_L_ES] = { "es", "Español",          0 },
    [FS_L_PT] = { "pt", "Português",        0 },
    [FS_L_FR] = { "fr", "Français",         0 },
    [FS_L_DE] = { "de", "Deutsch",          0 },
    [FS_L_IT] = { "it", "Italiano",         0 },
    [FS_L_JA] = { "ja", "日本語",              0 },
    [FS_L_PL] = { "pl", "Polski",           0 },
    [FS_L_UK] = { "uk", "Українська",       0 },
    [FS_L_TR] = { "tr", "Türkçe",           0 },
    [FS_L_ID] = { "id", "Bahasa Indonesia", 0 },
    [FS_L_TH] = { "th", "ไทย",              0 },
    [FS_L_NL] = { "nl", "Nederlands",       0 },
    [FS_L_SV] = { "sv", "Svenska",          0 },
    [FS_L_DA] = { "da", "Dansk",            0 },
    [FS_L_NO] = { "no", "Norsk",            0 },
    [FS_L_FI] = { "fi", "Suomi",            0 },
    [FS_L_CS] = { "cs", "Čeština",          0 },
    [FS_L_RO] = { "ro", "Română",           0 },
    [FS_L_HE] = { "he", "עברית",            1 },
    [FS_L_AR] = { "ar", "العربية",          1 },
};

#endif
