import { CSSProperties } from 'react';
import { StringId } from '../localization/strings';
import { useLocalization } from '../contexts/LocalizationContext';

interface TextProps {
  id: StringId;
  style?: CSSProperties;
  className?: string;
}

/**
 * Text component for displaying localized strings
 * Usage: <Text id="login" style={{...}} />
 */
export const Text = ({ id, style, className }: TextProps) => {
  const { t } = useLocalization();
  return <span style={style} className={className}>{t(id)}</span>;
};

