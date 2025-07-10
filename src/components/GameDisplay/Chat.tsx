import React, { useState, useEffect, useRef } from 'react';
import { useServer } from '../../contexts/ServerContext';
import { useAuth } from '../../contexts/AuthContext';

export const Chat = () => {
    const server = useServer();
    const { game, sendMessage, chatMessages } = server;
    const { user_id } = useAuth();
    const [message, setMessage] = useState('');
    const [isExpanded, setIsExpanded] = useState(false);
    const [isInputFocused, setIsInputFocused] = useState(false);
    const messagesEndRef = useRef<HTMLDivElement>(null);
    const inputRef = useRef<HTMLInputElement>(null);
    
    const scrollToBottom = () => {
        messagesEndRef.current?.scrollIntoView({ behavior: 'smooth' });
    };

    useEffect(() => {
        scrollToBottom();
    }, [chatMessages]);

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

    if (!game || !game.self) {
        return null; // Don't show chat for spectators or if game is not loaded
    }

    return (
        <div style={{ 
            position: 'absolute', 
            bottom: '220px', 
            right: '20px', 
            width: isExpanded ? '300px' : '200px',
            height: isExpanded ? '400px' : '50px',
            backgroundColor: 'rgba(0, 0, 0, 0.7)',
            border: '2px solid #ccc',
            borderRadius: '10px',
            display: 'flex',
            flexDirection: 'column',
            overflow: 'hidden',
            transition: 'all 0.3s ease',
            zIndex: 1000
        }}>
            {/* Chat Header */}
            <div 
                style={{
                    padding: '10px',
                    backgroundColor: 'rgba(0, 0, 0, 0.9)',
                    color: 'white',
                    fontSize: '14px',
                    fontWeight: 'bold',
                    cursor: 'pointer',
                    borderBottom: isExpanded ? '1px solid #ccc' : 'none'
                }}
                onClick={() => setIsExpanded(!isExpanded)}
            >
                Chat {isExpanded ? '▼' : '▲'}
            </div>

            {/* Chat Messages */}
            {isExpanded && (
                <>
                    <div style={{
                        flex: 1,
                        padding: '10px',
                        overflowY: 'auto',
                        maxHeight: '280px'
                    }}>
                        {chatMessages.map((msg, index) => (
                            <div key={index} style={{
                                marginBottom: '8px',
                                padding: '5px',
                                backgroundColor: msg.user_id === user_id ? 'rgba(0, 100, 200, 0.3)' : 'rgba(255, 255, 255, 0.1)',
                                borderRadius: '5px',
                                color: 'white',
                                fontSize: '12px',
                                wordWrap: 'break-word'
                            }}>
                                <div style={{ fontWeight: 'bold', marginBottom: '2px' }}>
                                    {msg.sender_name || 'Unknown'}
                                </div>
                                <div>
                                    {msg.message}
                                </div>
                                <div style={{ fontSize: '10px', color: '#ccc', marginTop: '2px' }}>
                                    {new Date(msg.created_at).toLocaleTimeString()}
                                </div>
                            </div>
                        ))}
                        <div ref={messagesEndRef} />
                    </div>

                    {/* Chat Input */}
                    <form onSubmit={handleSubmit} style={{
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