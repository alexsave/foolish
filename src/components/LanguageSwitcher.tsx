import { useLocalization, Language } from '../contexts/LocalizationContext';

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

// Soviet-style flag icons - proper flags with Soviet color palette
// Red (#B32929) replaces reds, Cream (#F5E6C8) replaces whites, Black (#0A0A0A) replaces blues
const SovietFlag: React.FC<{ code: Language; size?: number }> = ({ code, size = 45 }) => {
  const width = size;
  const height = size * (2/3); // 2:3 height:width ratio
  const style: React.CSSProperties = {
    width,
    height,
    display: 'block',
  };

  switch (code) {
    case 'en':
      // US flag - 13 stripes, star canton (2:3 ratio = 100:150)
      return (
        <svg viewBox="0 0 150 100" style={style}>
          {/* 13 stripes alternating red and cream */}
          {[0,1,2,3,4,5,6,7,8,9,10,11,12].map(i => (
            <rect key={i} x="0" y={i * 7.69} width="150" height="7.69" fill={i % 2 === 0 ? '#B32929' : '#F5E6C8'} />
          ))}
          {/* Star canton - black instead of blue */}
          <rect x="0" y="0" width="60" height="53.85" fill="#0A0A0A" />
          {/* 50 stars in 9 rows (6-5-6-5-6-5-6-5-6) */}
          {[0,1,2,3,4,5,6,7,8].map(row => {
            const cols = row % 2 === 0 ? 6 : 5;
            const offsetX = row % 2 === 0 ? 5 : 10;
            return Array.from({length: cols}, (_, col) => (
              <polygon
                key={`${row}-${col}`}
                points="0,-2.5 0.75,-0.75 2.5,-0.75 1.25,0.5 1.75,2.5 0,1.25 -1.75,2.5 -1.25,0.5 -2.5,-0.75 -0.75,-0.75"
                fill="#F5E6C8"
                transform={`translate(${offsetX + col * 10}, ${5 + row * 5.4})`}
              />
            ));
          })}
        </svg>
      );
    case 'ru':
      // Russian Federation flag - tricolor with Soviet color palette (2:3 ratio)
      // White -> Cream, Blue -> Black, Red -> Soviet Red
      return (
        <svg viewBox="0 0 150 100" style={style}>
          <rect x="0" y="0" width="150" height="33.33" fill="#F5E6C8" />
          <rect x="0" y="33.33" width="150" height="33.33" fill="#0A0A0A" />
          <rect x="0" y="66.66" width="150" height="33.34" fill="#B32929" />
        </svg>
      );
    case 'ko':
      // South Korean flag - Taegukgi with proper taeguk and trigrams (2:3 ratio)
      return (
        <svg viewBox="0 0 150 100" style={style}>
          {/* Cream field */}
          <rect x="0" y="0" width="150" height="100" fill="#F5E6C8" />
          
          {/* Taeguk (yin-yang) - centered */}
          <g transform="translate(75, 50)">
            {/* Red (yang) half */}
            <path d="M 0,-22 A 22,22 0 0,1 0,22 A 11,11 0 0,1 0,0 A 11,11 0 0,0 0,-22" fill="#B32929" />
            {/* Black (yin) half - replacing blue */}
            <path d="M 0,22 A 22,22 0 0,1 0,-22 A 11,11 0 0,1 0,0 A 11,11 0 0,0 0,22" fill="#0A0A0A" />
          </g>
          
          {/* Trigrams - positioned closer to taeguk */}
          {/* Geon (☰) - top left - 3 solid bars */}
          <g transform="translate(38, 28) rotate(-56.31)" stroke="#0A0A0A" strokeWidth="3.5" strokeLinecap="butt">
            <line x1="-10" y1="-6" x2="10" y2="-6" />
            <line x1="-10" y1="0" x2="10" y2="0" />
            <line x1="-10" y1="6" x2="10" y2="6" />
          </g>
          
          {/* Gon (☷) - bottom right - 3 broken bars */}
          <g transform="translate(112, 72) rotate(-56.31)" stroke="#0A0A0A" strokeWidth="3.5" strokeLinecap="butt">
            <line x1="-10" y1="-6" x2="-2" y2="-6" />
            <line x1="2" y1="-6" x2="10" y2="-6" />
            <line x1="-10" y1="0" x2="-2" y2="0" />
            <line x1="2" y1="0" x2="10" y2="0" />
            <line x1="-10" y1="6" x2="-2" y2="6" />
            <line x1="2" y1="6" x2="10" y2="6" />
          </g>
          
          {/* Gam (☵) - top right - broken, solid, broken */}
          <g transform="translate(112, 28) rotate(56.31)" stroke="#0A0A0A" strokeWidth="3.5" strokeLinecap="butt">
            <line x1="-10" y1="-6" x2="-2" y2="-6" />
            <line x1="2" y1="-6" x2="10" y2="-6" />
            <line x1="-10" y1="0" x2="10" y2="0" />
            <line x1="-10" y1="6" x2="-2" y2="6" />
            <line x1="2" y1="6" x2="10" y2="6" />
          </g>
          
          {/* Ri (☲) - bottom left - solid, broken, solid */}
          <g transform="translate(38, 72) rotate(56.31)" stroke="#0A0A0A" strokeWidth="3.5" strokeLinecap="butt">
            <line x1="-10" y1="-6" x2="10" y2="-6" />
            <line x1="-10" y1="0" x2="-2" y2="0" />
            <line x1="2" y1="0" x2="10" y2="0" />
            <line x1="-10" y1="6" x2="10" y2="6" />
          </g>
        </svg>
      );
    default:
      return null;
  }
};

export const LanguageSwitcher = () => {
  const { language, setLanguage } = useLocalization();

  return (
    <div className="fixed flex gap-sm" style={{ bottom: '10px', right: '10px', zIndex: 1000 }}>
      {LANGUAGES.map(({ code, flag, label }) => {
        const isActive = language === code;
        return (
          <button
            key={code}
            onClick={() => setLanguage(code)}
            disabled={isActive}
            className={`btn-language ${isActive ? 'btn-language--active' : ''}`}
          >
            {/* Soviet mode: CSS hides emoji content, shows SovietFlag */}
            {/* Default mode: CSS shows emoji content, hides SovietFlag */}
            <span className="btn-language__flag btn-language__flag--emoji">{flag}</span>
            <span className="btn-language__label btn-language__label--emoji">{label}</span>
            <span className="btn-language__flag--soviet"><SovietFlag code={code} size={40} /></span>
          </button>
        );
      })}
    </div>
  );
};

