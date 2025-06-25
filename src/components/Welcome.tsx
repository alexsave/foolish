import { useNavigate } from 'react-router-dom';

export const Welcome = () => {
    const navigate = useNavigate();

    return (
        <div>
            <p>FOOLISH</p>
            <button onClick={() => {
                navigate('/login');
            }}>
                Start
            </button>
        </div>
    );
};