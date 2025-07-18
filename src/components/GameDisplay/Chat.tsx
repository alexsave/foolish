import React, { useState, useEffect, useRef } from 'react';
import { useServer } from '../../contexts/ServerContext';
import { useAuth } from '../../contexts/AuthContext';

// Function to generate color from name hash
const getNameColor = (name: string): string => {
    let hash = 0;
    for (let i = 0; i < name.length; i++) {
        hash = name.charCodeAt(i) + ((hash << 5) - hash);
    }
    const color = Math.abs(hash).toString(16).substring(0, 6);
    return '#' + color.padStart(6, '0');
};

export const Chat = () => {
    const server = useServer();
    const { game, sendMessage, chatMessages } = server;
    const { user_id } = useAuth();
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
        scrollToBottom(true); // Smooth scroll for new messages
    }, [chatMessages]);

    // Jump to bottom immediately when chat is expanded
    useEffect(() => {
        if (isExpanded) {
            // Use setTimeout to ensure DOM is updated
            setTimeout(() => {
                scrollToBottom(false); // Instant jump to bottom
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

    // iOS viewport and keyboard handling
    useEffect(() => {
        const handleViewportChange = () => {
            const newHeight = window.innerHeight;
            const heightDifference = viewportHeight - newHeight;
            
            // Consider keyboard open if viewport shrunk by more than 150px
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

        // Listen for both window resize and visual viewport changes (iOS Safari)
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
            // Blur the input to hide keyboard on mobile
            inputRef.current?.blur();
        }
    };

    const handleToggle = () => {
        setIsExpanded(!isExpanded);
    };

    if (!game || !game.self) {
        return null; // Don't show chat for spectators or if game is not loaded
    }

    // Get the actual available height (accounting for keyboard)
    const availableHeight = isKeyboardOpen && window.visualViewport 
        ? window.visualViewport.height 
        : window.innerHeight;

    // Mobile styles
    if (isMobile) {
        if (!isExpanded) {
            // Small button on mobile
            return (
                <div 
                    data-chat-button
                    style={{
                        position: 'fixed',
                        top: '50%',
                        left: 'env(safe-area-inset-left, 0)',
                        transform: 'translateY(-50%)',
                        width: '60px',
                        height: '60px',
                        backgroundColor: 'rgba(0, 0, 0, 0.8)',
                        borderTop: '2px solid #ccc',
                        borderRight: '2px solid #ccc',
                        borderBottom: '2px solid #ccc',
                        borderLeft: 'none',
                        borderRadius: '0 8px 8px 0',
                        display: 'flex',
                        alignItems: 'center',
                        justifyContent: 'center',
                        cursor: 'pointer',
                        zIndex: 9999,
                        transition: 'all 0.3s ease',
                        touchAction: 'manipulation'
                    }}
                    onClick={handleToggle}
                >
                    <div style={{
                        color: 'white',
                        fontSize: '24px',
                        fontWeight: 'bold'
                    }}>
                        💬
                    </div>
                </div>
            );
        } else {
            // Fullscreen on mobile with proper viewport handling
            return (
                <div style={{
                    position: 'fixed',
                    top: 'max(10px, env(safe-area-inset-top, 0px))',
                    left: 'max(10px, env(safe-area-inset-left, 0px))',
                    right: 'max(10px, env(safe-area-inset-right, 0px))',
                    bottom: isKeyboardOpen && window.visualViewport 
                        ? `${window.innerHeight - window.visualViewport.height + 10}px`
                        : 'max(10px, env(safe-area-inset-bottom, 0px))',
                    backgroundColor: 'rgba(0, 0, 0, 0.9)',
                    display: 'flex',
                    flexDirection: 'column',
                    zIndex: 9999,
                    borderRadius: '10px',
                    border: '2px solid #ccc',
                    transition: isKeyboardOpen ? 'none' : 'all 0.3s ease',
                    transform: 'scale(1)',
                    opacity: 1,
                    touchAction: 'none'
                }}>
                    {/* Header */}
                    <div 
                        data-touch-interactive
                        style={{
                            padding: '15px',
                            backgroundColor: 'rgba(0, 0, 0, 1)',
                            color: 'white',
                            fontSize: '18px',
                            fontWeight: 'bold',
                            display: 'flex',
                            justifyContent: 'space-between',
                            alignItems: 'center',
                            borderBottom: '1px solid #ccc',
                            flexShrink: 0
                        }}
                    >
                        <span>Chat</span>
                        <button
                            onClick={handleToggle}
                            style={{
                                backgroundColor: 'transparent',
                                border: 'none',
                                color: 'white',
                                fontSize: '24px',
                                cursor: 'pointer'
                            }}
                        >
                            ✕
                        </button>
                    </div>

                    {/* Messages */}
                    <div 
                        data-chat-scrollable
                        style={{
                            flex: 1,
                            padding: '15px',
                            overflowY: 'auto',
                            touchAction: 'pan-y',
                            minHeight: 0 // Allow flex shrinking
                        }}
>
                        {chatMessages.map((msg, index) => {
                            const senderName = msg.sender_name || 'Unknown';
                            const nameColor = getNameColor(senderName);
                            
                            return (
                                <div key={index} style={{
                                    marginBottom: '12px',
                                    padding: '8px',
                                    color: 'white',
                                    fontSize: '14px',
                                    wordWrap: 'break-word'
                                }}>
                                    <span style={{ color: nameColor, fontWeight: 'bold' }}>
                                        {senderName}:
                                    </span>
                                    {' '}
                                    <span>{msg.message}</span>
                                    {' '}
                                    <span style={{ fontSize: '12px', color: '#ccc' }}>
                                        [{new Date(msg.created_at).toLocaleTimeString()}]
                                    </span>
                                </div>
                            );
                        })}
                        <div ref={messagesEndRef} />
                    </div>

                    {/* Input */}
                    <form 
                        data-touch-interactive
                        onSubmit={handleSubmit} 
                        style={{
                            padding: '15px',
                            paddingBottom: '15px',
                            borderTop: '1px solid #ccc',
                            display: 'flex',
                            gap: '10px',
                            backgroundColor: 'rgba(0, 0, 0, 0.95)',
                            flexShrink: 0
                        }}>
                        <input
                            ref={inputRef}
                            type="text"
                            value={message}
                            onChange={(e) => setMessage(e.target.value)}
                            onKeyPress={handleKeyPress}
                            onFocus={() => setIsInputFocused(true)}
                            onBlur={() => setIsInputFocused(false)}
                            placeholder="Type message..."
                            inputMode={isInputFocused ? 'text' : 'none'}
                            style={{
                                flex: 1,
                                padding: '10px',
                                borderRadius: '8px',
                                border: '1px solid #ccc',
                                fontSize: '16px'
                            }}
                            maxLength={1000}
                        />
                        <button
                            type="submit"
                            style={{
                                padding: '10px 15px',
                                backgroundColor: '#007bff',
                                color: 'white',
                                border: 'none',
                                borderRadius: '8px',
                                cursor: 'pointer',
                                fontSize: '14px'
                            }}
                            disabled={!message.trim()}
                        >
                            Send
                        </button>
                    </form>
                </div>
            );
        }
    }

    // Desktop styles - use button when collapsed, full chat when expanded
    if (!isExpanded) {
        // Button style on desktop (similar to mobile)
        return (
            <div 
                data-chat-button
                style={{
                    position: 'fixed',
                    top: '50%',
                    left: '0',
                    transform: 'translateY(-50%)',
                    width: '60px',
                    height: '60px',
                    backgroundColor: 'rgba(0, 0, 0, 0.8)',
                    borderTop: '2px solid #ccc',
                    borderRight: '2px solid #ccc',
                    borderBottom: '2px solid #ccc',
                    borderLeft: 'none',
                    borderRadius: '0 8px 8px 0',
                    display: 'flex',
                    alignItems: 'center',
                    justifyContent: 'center',
                    cursor: 'pointer',
                    zIndex: 9999,
                    transition: 'all 0.3s ease',
                    touchAction: 'manipulation'
                }}
                onClick={handleToggle}
            >
                <div style={{
                    color: 'white',
                    fontSize: '24px',
                    fontWeight: 'bold'
                }}>
                    💬
                </div>
            </div>
        );
    }

    // Expanded desktop chat
    return (
        <div style={{ 
            position: 'absolute', 
            bottom: '220px', 
            left: '20px', 
            width: '300px',
            height: '400px',
            backgroundColor: 'rgba(0, 0, 0, 0.7)',
            border: '2px solid #ccc',
            borderRadius: '10px',
            display: 'flex',
            flexDirection: 'column',
            overflow: 'hidden',
            transition: 'all 0.3s ease',
            zIndex: 9999,
            touchAction: 'none'
        }}>
            {/* Chat Header */}
            <div 
                data-touch-interactive
                style={{
                    padding: '10px',
                    backgroundColor: 'rgba(0, 0, 0, 0.9)',
                    color: 'white',
                    fontSize: '14px',
                    fontWeight: 'bold',
                    cursor: 'pointer',
                    borderBottom: '1px solid #ccc'
                }}
                onClick={handleToggle}
            >
                Chat ▼
            </div>

            {/* Chat Messages */}
            {isExpanded && (
                <>
                    <div 
                        data-chat-scrollable
                        style={{
                            flex: 1,
                            padding: '10px',
                            overflowY: 'auto',
                            maxHeight: '280px',
                            touchAction: 'pan-y'
                        }}
>
                        {chatMessages.map((msg, index) => {
                            const senderName = msg.sender_name || 'Unknown';
                            const nameColor = getNameColor(senderName);
                            
                            return (
                                <div key={index} style={{
                                    marginBottom: '8px',
                                    padding: '5px',
                                    color: 'white',
                                    fontSize: '12px',
                                    wordWrap: 'break-word'
                                }}>
                                    <span style={{ color: nameColor, fontWeight: 'bold' }}>
                                        {senderName}:
                                    </span>
                                    {' '}
                                    <span>{msg.message}</span>
                                    {' '}
                                    <span style={{ fontSize: '10px', color: '#ccc' }}>
                                        [{new Date(msg.created_at).toLocaleTimeString()}]
                                    </span>
                                </div>
                            );
                        })}
                        <div ref={messagesEndRef} />
                    </div>

                    {/* Chat Input */}
                    <form 
                        data-touch-interactive
                        onSubmit={handleSubmit} 
                        style={{
                            padding: '10px',
                            borderTop: '1px solid #ccc',
                            display: 'flex',
                            gap: '5px'
                        }}>
                        <input
                            ref={inputRef}
                            type="text"
                            value={message}
                            onChange={(e) => setMessage(e.target.value)}
                            onKeyPress={handleKeyPress}
                            onFocus={() => setIsInputFocused(true)}
                            onBlur={() => setIsInputFocused(false)}
                            placeholder="Type message..."
                            inputMode={isInputFocused ? 'text' : 'none'}
                            style={{
                                flex: 1,
                                padding: '5px',
                                borderRadius: '5px',
                                border: '1px solid #ccc',
                                fontSize: '16px' // Prevent iOS zoom by using 16px or larger
                            }}
                            maxLength={1000}
                        />
                        <button
                            type="submit"
                            style={{
                                padding: '5px 10px',
                                backgroundColor: '#007bff',
                                color: 'white',
                                border: 'none',
                                borderRadius: '5px',
                                cursor: 'pointer',
                                fontSize: '12px'
                            }}
                            disabled={!message.trim()}
                        >
                            Send
                        </button>
                    </form>
                </>
            )}
        </div>
    );
}; 