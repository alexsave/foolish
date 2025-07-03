import { useNavigate } from 'react-router-dom';

export const Welcome = () => {
    const navigate = useNavigate();

    return (
        <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', height: '100%', width: '100%' }}>

            <p style={{ fontSize: '48px', fontWeight: 'bold', color: 'rgb(239, 151, 28)' }}>FOOLISH</p>
            <button style={{padding: '10px 20px', fontSize: '16px', fontWeight: 'bold', cursor: 'pointer'}} onClick={() => { navigate('/login'); }}>
                Start
            </button>
        </div>
    );
};