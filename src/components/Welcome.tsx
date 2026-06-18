import { useState, useRef, useEffect } from 'react';
import { useAuth } from '../contexts/AuthContext';
import { generateFernPattern } from '../utils/fernFractal';
import { TexturedSurface } from './TexturedSurface';
import { WoolBackgroundLayer } from './WoolBackgroundLayer';
import { LanguageSwitcher } from './LanguageSwitcher';
import { Text } from './Text';
import { useLocalization } from '../contexts/LocalizationContext';
import Link from 'next/link';
import { useStyles } from '../contexts/StyleContext';
import { usernameUsesReservedPrefix } from '../common/botName';

export const Welcome = () => {
    const [name, setName] = useState('');
    const [password, setPassword] = useState('');
    const { signIn, signUp } = useAuth();
    const { t, language } = useLocalization();
    const styles = useStyles();
    const passwordRef = useRef<HTMLInputElement>(null);

    // For the en/ko locales, fill the brand title's glyphs with the cardback
    // fern texture (red outline, fern interior). The texture is generated lazily
    // and globally cached; until it resolves the title stays plain red.
    const useFernTitle = language === 'en' || language === 'ko';
    const [fernPattern, setFernPattern] = useState('');

    useEffect(() => {
        if (!useFernTitle) return;
        let cancelled = false;
        generateFernPattern()
            .then((url) => { if (!cancelled) setFernPattern(url); })
            .catch(() => { /* leave the plain-red fallback in place */ });
        return () => { cancelled = true; };
    }, [useFernTitle]);

    const useCustomMasking = styles.behavior.useCustomPasswordMasking;
    const maskedPassword = '#'.repeat(password.length);
    
    const handlePasswordKeyDown = (e: React.KeyboardEvent<HTMLInputElement>) => {
        if (!useCustomMasking) return;
        
        const input = e.currentTarget;
        const start = input.selectionStart ?? 0;
        const end = input.selectionEnd ?? 0;
        
        if (e.key === 'Backspace') {
            e.preventDefault();
            if (start !== end) {
                setPassword(password.slice(0, start) + password.slice(end));
                setTimeout(() => input.setSelectionRange(start, start), 0);
            } else if (start > 0) {
                setPassword(password.slice(0, start - 1) + password.slice(start));
                setTimeout(() => input.setSelectionRange(start - 1, start - 1), 0);
            }
        } else if (e.key === 'Delete') {
            e.preventDefault();
            if (start !== end) {
                setPassword(password.slice(0, start) + password.slice(end));
            } else if (start < password.length) {
                setPassword(password.slice(0, start) + password.slice(start + 1));
            }
            setTimeout(() => input.setSelectionRange(start, start), 0);
        } else if (e.key.length === 1 && !e.ctrlKey && !e.metaKey) {
            e.preventDefault();
            const newPassword = password.slice(0, start) + e.key + password.slice(end);
            setPassword(newPassword);
            setTimeout(() => input.setSelectionRange(start + 1, start + 1), 0);
        }
    };
    
    const handlePasswordPaste = (e: React.ClipboardEvent<HTMLInputElement>) => {
        if (!useCustomMasking) return;
        
        e.preventDefault();
        const input = e.currentTarget;
        const start = input.selectionStart ?? 0;
        const end = input.selectionEnd ?? 0;
        const pastedText = e.clipboardData.getData('text');
        
        const newPassword = password.slice(0, start) + pastedText + password.slice(end);
        setPassword(newPassword);
        setTimeout(() => input.setSelectionRange(start + pastedText.length, start + pastedText.length), 0);
    };

    const handleLogin = async (e: React.FormEvent) => {
        e.preventDefault();
        try {
            const { weakPassword } = await signIn(name, password);
            if (weakPassword) {
                alert(t('weak_password'));
            }
        } catch (error: any) {
            // localized headline; the raw server detail stays for debugging
            alert(`${t('login_failed')}: ${error.message}`);
        }
    };

    const handleSignUp = async (e: React.FormEvent) => {
        e.preventDefault();
        // The robot-emoji prefix is reserved for bots; reject it up front with a
        // localized message (the AuthContext guard + DB trigger also enforce it).
        if (usernameUsesReservedPrefix(name)) {
            alert(t('username_reserved'));
            return;
        }
        try {
            await signUp(name, password);
        } catch (error: any) {
            const msg = error?.message === 'USERNAME_RESERVED_PREFIX'
                ? t('username_reserved')
                : `${t('signup_failed')}: ${error.message}`;
            alert(msg);
        }
    };

    return (
        <div className="page page--centered page--full-viewport">
            <WoolBackgroundLayer />
            
            <p className="title title--brand z-content">
                <Text
                    id="foolish"
                    className={useFernTitle
                        ? `title-fern${fernPattern ? ' title-fern--loaded' : ''}`
                        : undefined}
                    style={useFernTitle && fernPattern
                        ? ({ '--fern-texture': `url(${fernPattern})` } as React.CSSProperties)
                        : undefined}
                />
            </p>
            
            <form onSubmit={handleLogin} className="flex flex-col items-center z-content" style={{ gap: '15px' }}>
                <div className="form-group">
                    <label htmlFor="username" className="form-label"><Text id="username" />:</label>
                    <input
                        id="username"
                        type="text"
                        value={name}
                        onChange={(e) => setName(e.target.value)}
                        autoComplete="username"
                        required
                        className="input-standard"
                    />
                </div>
                <div className="form-group">
                    <label htmlFor="password" className="form-label"><Text id="password" />:</label>
                    <input
                        ref={passwordRef}
                        id="password"
                        type={useCustomMasking ? "text" : "password"}
                        value={useCustomMasking ? maskedPassword : password}
                        onChange={useCustomMasking ? undefined : (e) => setPassword(e.target.value)}
                        onKeyDown={handlePasswordKeyDown}
                        onPaste={handlePasswordPaste}
                        autoComplete={useCustomMasking ? "off" : "current-password"}
                        required
                        className="input-standard"
                    />
                </div>
                <div className="form-row form-row--actions">
                    <TexturedSurface as="button" seed={0.5} type="submit" className="btn-wood btn-wood--md">
                        <span className="btn-wood-text"><Text id="login" /></span>
                    </TexturedSurface>
                    <TexturedSurface as="button" seed={0.9} type="button" onClick={handleSignUp} className="btn-wood btn-wood--md">
                        <span className="btn-wood-text"><Text id="sign_up" /></span>
                    </TexturedSurface>
                </div>
            </form>
            
            <LanguageSwitcher />
            
            <Link href="/about" className="link link--bottom">
                <Text id="about" />
            </Link>
        </div>
    );
};
