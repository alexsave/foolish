import React, { useState, useEffect, useRef } from 'react';
import { useServer } from '../../contexts/ServerContext';
import { useLocalization } from '../../contexts/LocalizationContext';
import { SovietIcon } from '../SovietIcon';

export const Chat = () => {
    const server = useServer();
    const { view, sendMessage, chatMessages } = server;
    const { t } = useLocalization();
    const [message, setMessage] = useState('');
    const [isExpanded, setIsExpanded] = useState(false);
    const [isInputFocused, setIsInputFocused] = useState(false);
    const [isMobile, setIsMobile] = useState(false);
    const [viewportHeight, setViewportHeight] = useState(window.innerHeight);
    const [isKeyboardOpen, setIsKeyboardOpen] = useState(false);
    const messagesContainerRef = useRef<HTMLDivElement>(null);
    const inputRef = useRef<HTMLInputElement>(null);

    const scrollToBottom = (smooth = false) => {
        if (!messagesContainerRef.current) return;
        
        const container = messagesContainerRef.current;
        const scrollTarget = container.scrollHeight - container.clientHeight;
        
        if (smooth) {
            container.scrollTo({ top: scrollTarget, behavior: 'smooth' });
        } else {
            container.scrollTop = scrollTarget;
        }
    };

    useEffect(() => {
        scrollToBottom(true);
    }, [chatMessages]);

    useEffect(() => {
        if (isExpanded) {
            setTimeout(() => {
                scrollToBottom(false);
            }, 0);
        }
    }, [isExpanded]);

    useEffect(() => {
        const checkMobile = () => {
            setIsMobile(window.innerWidth <= 768);
        };
        checkMobile();
        window.addEventListener('resize', checkMobile);
        return () => window.removeEventListener('resize', checkMobile);
    }, []);

    useEffect(() => {
        const handleViewportChange = () => {
            const newHeight = window.innerHeight;
            const heightDifference = viewportHeight - newHeight;
            setIsKeyboardOpen(heightDifference > 150);
            setViewportHeight(newHeight);
        };

        const handleVisualViewportChange = () => {
            if (window.visualViewport) {
                const newHeight = window.visualViewport.height;
                const heightDifference = viewportHeight - newHeight;
                setIsKeyboardOpen(heightDifference > 150);
            }
        };

        window.addEventListener('resize', handleViewportChange);
        if (window.visualViewport) {
            window.visualViewport.addEventListener('resize', handleVisualViewportChange);
        }

        return () => {
            window.removeEventListener('resize', handleViewportChange);
            if (window.visualViewport) {
                window.visualViewport.removeEventListener('resize', handleVisualViewportChange);
            }
        };
    }, [viewportHeight]);

    const handleSubmit = async (e: React.FormEvent) => {
        e.preventDefault();
        if (message.trim()) {
            try {
                await sendMessage(message.trim());
                setMessage('');
            } catch (error) {
                console.error('Failed to send message:', error);
            }
        }
    };

    const handleKeyPress = (e: React.KeyboardEvent) => {
        if (e.key === 'Enter' && !e.shiftKey) {
            e.preventDefault();
            handleSubmit(e);
            inputRef.current?.blur();
        }
    };

    const handleToggle = () => {
        setIsExpanded(!isExpanded);
    };

    if (!view || view.mySeat < 0) {
        return null;
    }

    // Collapsed state - show toggle button
    if (!isExpanded) {
        return (
            <div 
                className="chat-toggle"
                data-chat-button
                onClick={handleToggle}
            >
                <div className="chat-toggle__icon">
                    {/* SovietIcon handles theme-switching internally */}
                    <SovietIcon name="telephone" size={28} />
                </div>
            </div>
        );
    }

    // Expanded: one panel for both layouts. On a phone each of these blocks
    // also carries its --mobile modifier, the header has a close button (on
    // desktop the whole header is the toggle), and the panel rides above the
    // keyboard.
    const block = (name: string) => (isMobile ? `${name} ${name}--mobile` : name);
    const keyboardStyle = isMobile && isKeyboardOpen && window.visualViewport
        ? { bottom: `${window.innerHeight - window.visualViewport.height + 10}px`, transition: 'none' }
        : undefined;

    return (
        <div className={block('chat')} style={keyboardStyle}>
            {isMobile ? (
                <div className={block('chat__header')} data-touch-interactive>
                    <span>{t('chat')}</span>
                    <button className="chat__close-btn" onClick={handleToggle}>✕</button>
                </div>
            ) : (
                <div className={block('chat__header')} onClick={handleToggle} data-touch-interactive>
                    {t('chat')} ▼
                </div>
            )}

            <div ref={messagesContainerRef} className={block('chat__messages')} data-chat-scrollable>
                {chatMessages.map((msg, index) => (
                    <div key={index} className={block('chat__message')}>
                        <span className="chat__message-sender">{msg.sender_name || 'Unknown'}:</span>
                        {' '}
                        <span className="chat__message-text">{msg.message}</span>
                        {' '}
                        <span className={block('chat__message-time')}>
                            [{new Date(msg.created_at).toLocaleTimeString()}]
                        </span>
                    </div>
                ))}
            </div>

            <form className={block('chat__form')} onSubmit={handleSubmit} data-touch-interactive>
                <input
                    ref={inputRef}
                    className={block('chat__input')}
                    type="text"
                    value={message}
                    onChange={(e) => setMessage(e.target.value)}
                    onKeyPress={handleKeyPress}
                    onFocus={() => setIsInputFocused(true)}
                    onBlur={() => setIsInputFocused(false)}
                    placeholder={t('type_message')}
                    inputMode={isInputFocused ? 'text' : 'none'}
                    maxLength={1000}
                />
                <button className={block('chat__submit')} type="submit" disabled={!message.trim()}>
                    {t('send')}
                </button>
            </form>
        </div>
    );
};
