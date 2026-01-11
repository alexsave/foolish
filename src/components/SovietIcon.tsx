import { useTheme } from '../contexts/ThemeContext';

type IconName = 
  | 'bot' 
  | 'person' 
  | 'ready' 
  | 'not-ready' 
  | 'shield' 
  | 'sword' 
  | 'crown' 
  | 'celebration'
  | 'fool'
  | 'medal-1'
  | 'medal-2'
  | 'medal-3'
  | 'spade'
  | 'heart'
  | 'club'
  | 'diamond'
  | 'telephone';

interface SovietIconProps {
  name: IconName;
  size?: number;
  className?: string;
  style?: React.CSSProperties;
  color?: string;
}

// Soviet-style flat icons as inline SVGs
const sovietIcons: Record<IconName, (size: number, color?: string) => JSX.Element> = {
  'bot': (size) => (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none">
      {/* CPU - square chip with wires/pins */}
      <rect x="6" y="6" width="12" height="12" fill="#B32929" stroke="#0A0A0A" strokeWidth="2"/>
      {/* Inner circuit pattern */}
      <rect x="9" y="9" width="6" height="6" fill="#0A0A0A"/>
      {/* Top pins */}
      <line x1="9" y1="6" x2="9" y2="2" stroke="#0A0A0A" strokeWidth="2"/>
      <line x1="12" y1="6" x2="12" y2="2" stroke="#0A0A0A" strokeWidth="2"/>
      <line x1="15" y1="6" x2="15" y2="2" stroke="#0A0A0A" strokeWidth="2"/>
      {/* Bottom pins */}
      <line x1="9" y1="18" x2="9" y2="22" stroke="#0A0A0A" strokeWidth="2"/>
      <line x1="12" y1="18" x2="12" y2="22" stroke="#0A0A0A" strokeWidth="2"/>
      <line x1="15" y1="18" x2="15" y2="22" stroke="#0A0A0A" strokeWidth="2"/>
      {/* Left pins */}
      <line x1="6" y1="9" x2="2" y2="9" stroke="#0A0A0A" strokeWidth="2"/>
      <line x1="6" y1="12" x2="2" y2="12" stroke="#0A0A0A" strokeWidth="2"/>
      <line x1="6" y1="15" x2="2" y2="15" stroke="#0A0A0A" strokeWidth="2"/>
      {/* Right pins */}
      <line x1="18" y1="9" x2="22" y2="9" stroke="#0A0A0A" strokeWidth="2"/>
      <line x1="18" y1="12" x2="22" y2="12" stroke="#0A0A0A" strokeWidth="2"/>
      <line x1="18" y1="15" x2="22" y2="15" stroke="#0A0A0A" strokeWidth="2"/>
    </svg>
  ),
  'person': (size) => (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none">
      <circle cx="12" cy="7" r="4" fill="#B32929" stroke="#0A0A0A" strokeWidth="2"/>
      <path d="M4 21V17C4 14.7909 5.79086 13 8 13H16C18.2091 13 20 14.7909 20 17V21" fill="#B32929" stroke="#0A0A0A" strokeWidth="2"/>
    </svg>
  ),
  'ready': (size) => (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none">
      <path d="M3 12L9 18L21 6" stroke="#B32929" strokeWidth="4" strokeLinecap="square" strokeLinejoin="miter"/>
    </svg>
  ),
  'not-ready': (size) => (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none">
      <path d="M4 4L20 20M20 4L4 20" stroke="#0A0A0A" strokeWidth="4" strokeLinecap="square"/>
    </svg>
  ),
  'shield': (size) => (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none">
      <path d="M12 2L3 6V12C3 17 7 21 12 22C17 21 21 17 21 12V6L12 2Z" fill="#B32929" stroke="#0A0A0A" strokeWidth="2"/>
      <path d="M12 6L12 18" stroke="#0A0A0A" strokeWidth="2"/>
      <path d="M6 10H18" stroke="#0A0A0A" strokeWidth="2"/>
    </svg>
  ),
  'sword': (size) => (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none">
      <path d="M19 3L5 17" stroke="#B32929" strokeWidth="3" strokeLinecap="square"/>
      <path d="M15 3H19V7" stroke="#B32929" strokeWidth="3" strokeLinecap="square"/>
      <path d="M3 19L7 15" stroke="#0A0A0A" strokeWidth="3" strokeLinecap="square"/>
      <rect x="2" y="18" width="4" height="4" fill="#0A0A0A"/>
    </svg>
  ),
  'crown': (size) => (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none">
      <path d="M2 17L4 7L8 12L12 5L16 12L20 7L22 17H2Z" fill="#B32929" stroke="#0A0A0A" strokeWidth="2"/>
      <rect x="2" y="17" width="20" height="4" fill="#B32929" stroke="#0A0A0A" strokeWidth="2"/>
    </svg>
  ),
  'celebration': (size) => (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none">
      <polygon points="12,2 15,9 22,9 16,14 18,21 12,17 6,21 8,14 2,9 9,9" fill="#B32929" stroke="#0A0A0A" strokeWidth="2"/>
    </svg>
  ),
  'fool': (size) => (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none">
      <circle cx="12" cy="10" r="6" fill="#B32929" stroke="#0A0A0A" strokeWidth="2"/>
      <circle cx="9" cy="8" r="1.5" fill="#0A0A0A"/>
      <circle cx="15" cy="8" r="1.5" fill="#0A0A0A"/>
      <path d="M9 12C9 12 10.5 14 12 14C13.5 14 15 12 15 12" stroke="#0A0A0A" strokeWidth="2"/>
      <path d="M6 4L8 7" stroke="#B32929" strokeWidth="2"/>
      <path d="M18 4L16 7" stroke="#B32929" strokeWidth="2"/>
      <path d="M12 17V21" stroke="#0A0A0A" strokeWidth="2"/>
      <circle cx="6" cy="3" r="2" fill="#B32929" stroke="#0A0A0A" strokeWidth="1"/>
      <circle cx="18" cy="3" r="2" fill="#B32929" stroke="#0A0A0A" strokeWidth="1"/>
    </svg>
  ),
  'medal-1': (size) => (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none">
      <circle cx="12" cy="14" r="8" fill="#B32929" stroke="#0A0A0A" strokeWidth="2"/>
      <path d="M8 2L12 8L16 2" stroke="#B32929" strokeWidth="3"/>
      <text x="12" y="18" textAnchor="middle" fill="#F5E6C8" fontSize="10" fontWeight="bold" fontFamily="sans-serif">1</text>
    </svg>
  ),
  'medal-2': (size) => (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none">
      <circle cx="12" cy="14" r="8" fill="#0A0A0A" stroke="#B32929" strokeWidth="2"/>
      <path d="M8 2L12 8L16 2" stroke="#0A0A0A" strokeWidth="3"/>
      <text x="12" y="18" textAnchor="middle" fill="#F5E6C8" fontSize="10" fontWeight="bold" fontFamily="sans-serif">2</text>
    </svg>
  ),
  'medal-3': (size) => (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none">
      <circle cx="12" cy="14" r="8" fill="#0A0A0A" stroke="#B32929" strokeWidth="2"/>
      <path d="M8 2L12 8L16 2" stroke="#0A0A0A" strokeWidth="3"/>
      <text x="12" y="18" textAnchor="middle" fill="#F5E6C8" fontSize="10" fontWeight="bold" fontFamily="sans-serif">3</text>
    </svg>
  ),
  'spade': (size, color = '#0A0A0A') => (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none">
      <path d="M12 1C12 1 6 8 6 12C6 14.5 7.5 16 9.5 16C10.5 16 11.5 15.5 12 14.5C12.5 15.5 13.5 16 14.5 16C16.5 16 18 14.5 18 12C18 8 12 1 12 1Z" fill={color} stroke="#F5E6C8" strokeWidth="1"/>
      <path d="M12 15V21" stroke={color} strokeWidth="1.5"/>
      <path d="M9 21H15" stroke={color} strokeWidth="1.5"/>
    </svg>
  ),
  'heart': (size, color = '#B32929') => (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none">
      <path d="M12 20L5 12C3.5 10 3.5 7 6 5C8 3.5 10 5 12 7C14 5 16 3.5 18 5C20.5 7 20.5 10 19 12L12 20Z" fill={color} stroke="#F5E6C8" strokeWidth="1"/>
    </svg>
  ),
  'club': (size, color = '#0A0A0A') => (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none">
      {/* Top circle */}
      <circle cx="12" cy="6" r="4" fill={color} stroke="#F5E6C8" strokeWidth="1"/>
      {/* Bottom left circle */}
      <circle cx="7" cy="12" r="4" fill={color} stroke="#F5E6C8" strokeWidth="1"/>
      {/* Bottom right circle */}
      <circle cx="17" cy="12" r="4" fill={color} stroke="#F5E6C8" strokeWidth="1"/>
      {/* Center fill to connect circles */}
      <path d="M12 6L7 12L12 14L17 12L12 6Z" fill={color}/>
      {/* Stem */}
      <path d="M10 13L10 21L14 21L14 13Z" fill={color} stroke="#F5E6C8" strokeWidth="1"/>
    </svg>
  ),
  'diamond': (size, color = '#B32929') => (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none">
      <path d="M12 2L20 12L12 22L4 12L12 2Z" fill={color} stroke="#F5E6C8" strokeWidth="1"/>
    </svg>
  ),
  'telephone': (size) => (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none">
      {/* Simple handset shape */}
      <rect x="3" y="4" width="6" height="8" fill="#B32929"/>
      <rect x="15" y="4" width="6" height="8" fill="#B32929"/>
      <rect x="6" y="8" width="12" height="4" fill="#B32929"/>
    </svg>
  ),
};

// Default emojis for non-Soviet theme
const defaultEmojis: Record<IconName, string> = {
  'bot': '🤖',
  'person': '👤',
  'ready': '🟢',
  'not-ready': '🔴',
  'shield': '🛡️',
  'sword': '⚔️',
  'crown': '👑',
  'celebration': '🎉',
  'fool': '🃏',
  'medal-1': '🥇',
  'medal-2': '🥈',
  'medal-3': '🥉',
  'spade': '♠️',
  'heart': '♥️',
  'club': '♣️',
  'diamond': '♦️',
  'telephone': '📞',
};

export const SovietIcon: React.FC<SovietIconProps> = ({ 
  name, 
  size = 16, 
  className = '',
  style = {},
  color
}) => {
  const { isSoviet } = useTheme();

  if (isSoviet) {
    return (
      <span 
        className={className} 
        style={{ 
          display: 'inline-flex', 
          alignItems: 'center', 
          justifyContent: 'center',
          verticalAlign: 'middle',
          ...style 
        }}
      >
        {sovietIcons[name](size, color)}
      </span>
    );
  }

  return (
    <span className={className} style={style}>
      {defaultEmojis[name]}
    </span>
  );
};

// Suit icon helper component
export const SuitIcon: React.FC<{ suit: number; size?: number; style?: React.CSSProperties }> = ({ 
  suit, 
  size = 24,
  style = {}
}) => {
  const suitNames: Record<number, IconName> = {
    0: 'spade',
    1: 'heart',
    2: 'club',
    3: 'diamond',
  };
  
  const suitColors: Record<number, string> = {
    0: '#0A0A0A', // black
    1: '#B32929', // red
    2: '#0A0A0A', // black
    3: '#B32929', // red
  };

  const iconName = suitNames[suit];
  if (!iconName) return <span>?</span>;

  return <SovietIcon name={iconName} size={size} color={suitColors[suit]} style={style} />;
};

// Helper component for rank display
export const RankIcon: React.FC<{ rank: number; totalPlayers: number; size?: number }> = ({ 
  rank, 
  totalPlayers,
  size = 24 
}) => {
  const { isSoviet } = useTheme();

  // The fool (last place)
  if (rank === totalPlayers) {
    return <SovietIcon name="fool" size={size} />;
  }

  if (rank === 1) return <SovietIcon name="medal-1" size={size} />;
  if (rank === 2) return <SovietIcon name="medal-2" size={size} />;
  if (rank === 3) return <SovietIcon name="medal-3" size={size} />;

  // For ranks 4+, just show the number
  if (isSoviet) {
    return (
      <span style={{ 
        display: 'inline-flex',
        alignItems: 'center',
        justifyContent: 'center',
        width: size,
        height: size,
        backgroundColor: '#0A0A0A',
        color: '#F5E6C8',
        fontWeight: 'bold',
        fontSize: size * 0.5,
      }}>
        {rank}
      </span>
    );
  }

  return <span>#{rank}</span>;
};
