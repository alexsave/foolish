import { useState, useRef } from 'react';
import { useAuth } from '../contexts/AuthContext';
import { TexturedSurface } from './TexturedSurface';
import { WoolBackgroundLayer } from './WoolBackgroundLayer';
import { LanguageSwitcher } from './LanguageSwitcher';
import { Text } from './Text';
import Link from 'next/link';
import { useStyles } from '../contexts/StyleContext';

export const Welcome = () => {
    const [name, setName] = useState('');
    const [password, setPassword] = useState('');
    const { signIn, signUp } = useAuth();
    const styles = useStyles();
    const passwordRef = useRef<HTMLInputElement>(null);

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
                alert('Weak password');
            }
        } catch (error: any) {
            alert(error.message);
        }
    };

    const handleSignUp = async (e: React.FormEvent) => {
        e.preventDefault();
        try {
            await signUp(name, password);
        } catch (error: any) {
            alert(error.message);
        }
    };

    return (
        <div className="page page--centered page--full-viewport">
            <WoolBackgroundLayer />
            
            <p className="title title--brand z-content">
                <Text id="foolish" />
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
