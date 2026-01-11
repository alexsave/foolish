import React, { useState, useEffect, useRef } from 'react';
import { useServer } from '../../contexts/ServerContext';
import { useAuth } from '../../contexts/AuthContext';
import { useLocalization } from '../../contexts/LocalizationContext';
import { SovietIcon } from '../SovietIcon';

export const Chat = () => {
    const server = useServer();
    const { game, sendMessage, chatMessages } = server;
    const { user_id } = useAuth();
    const { t } = useLocalization();
    const [message, setMessage] = useState('');
    const [isExpanded, setIsExpanded] = useState(false);
    const [isInputFocused, setIsInputFocused] = useState(false);
    const [isMobile, setIsMobile] = useState(false);
    const [viewportHeight, setViewportHeight] = useState(window.innerHeight);
    const [isKeyboardOpen, setIsKeyboardOpen] = useState(false);
    const messagesEndRef = useRef<HTMLDivElement>(null);
    const inputRef = useRef<HTMLInputElement>(null);

    const scrollToBottom = (smooth = false) => {
        if (smooth) {
            messagesEndRef.current?.scrollIntoView({ behavior: 'smooth' });
        } else {
            messagesEndRef.current?.scrollIntoView({ behavior: 'instant' as ScrollBehavior });
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

    if (!game || !game.self) {
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

    // Mobile expanded
    if (isMobile) {
        const keyboardStyle = isKeyboardOpen && window.visualViewport 
            ? { bottom: `${window.innerHeight - window.visualViewport.height + 10}px`, transition: 'none' }
            : {};

        return (
            <div className="chat chat--mobile" style={keyboardStyle}>
                <div className="chat__header chat__header--mobile" data-touch-interactive>
                    <span>{t('chat')}</span>
                    <button className="chat__close-btn" onClick={handleToggle}>✕</button>
                </div>

                <div className="chat__messages chat__messages--mobile" data-chat-scrollable>
                    {chatMessages.map((msg, index) => (
                        <div key={index} className="chat__message chat__message--mobile">
                            <span className="chat__message-sender">{msg.sender_name || 'Unknown'}:</span>
                            {' '}
                            <span className="chat__message-text">{msg.message}</span>
                            {' '}
                            <span className="chat__message-time chat__message-time--mobile">
                                [{new Date(msg.created_at).toLocaleTimeString()}]
                            </span>
                        </div>
                    ))}
                    <div ref={messagesEndRef} />
                </div>

                <form className="chat__form chat__form--mobile" onSubmit={handleSubmit} data-touch-interactive>
                    <input
                        ref={inputRef}
                        className="chat__input chat__input--mobile"
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
                    <button className="chat__submit chat__submit--mobile" type="submit" disabled={!message.trim()}>
                        {t('send')}
                    </button>
                </form>
            </div>
        );
    }

    // Desktop expanded
    return (
        <div className="chat">
            <div className="chat__header" onClick={handleToggle} data-touch-interactive>
                {t('chat')} ▼
            </div>

            <div className="chat__messages" data-chat-scrollable>
                {chatMessages.map((msg, index) => (
                    <div key={index} className="chat__message">
                        <span className="chat__message-sender">{msg.sender_name || 'Unknown'}:</span>
                        {' '}
                        <span className="chat__message-text">{msg.message}</span>
                        {' '}
                        <span className="chat__message-time">
                            [{new Date(msg.created_at).toLocaleTimeString()}]
                        </span>
                    </div>
                ))}
                <div ref={messagesEndRef} />
            </div>

            <form className="chat__form" onSubmit={handleSubmit} data-touch-interactive>
                <input
                    ref={inputRef}
                    className="chat__input"
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
                <button className="chat__submit" type="submit" disabled={!message.trim()}>
                    {t('send')}
                </button>
            </form>
        </div>
    );
};
