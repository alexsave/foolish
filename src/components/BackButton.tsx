import { useNavigate } from 'react-router-dom';
import { useIconButtonStyle } from '../hooks/useIconButtonStyle';

export const BackButton = () => {
    const navigate = useNavigate();
    const { buttonBaseStyle, woodButtonStyle, woodButtonHoverStyle, iconStyle } = useIconButtonStyle();

    return (
        <button
            onClick={() => navigate('/dashboard')}
            style={{
                ...buttonBaseStyle,
                left: '10px',
                overflow: 'hidden',
            }}
            onMouseEnter={(e) => {
                Object.assign(e.currentTarget.style, woodButtonHoverStyle);
            }}
            onMouseLeave={(e) => {
                Object.assign(e.currentTarget.style, woodButtonStyle);
            }}
        >
            <span style={iconStyle}>{'<'}</span>
        </button>
    );
};

