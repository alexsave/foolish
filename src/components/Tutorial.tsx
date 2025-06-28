import { useNavigate } from "react-router-dom";

export const Tutorial = () => { 
    const navigate = useNavigate();
    return <div>
        <h1>Tutorial</h1>
        <p>Insert a cool tutorial here with a sample game and everything</p>
        <button onClick={() => navigate('/dashboard')}>Go to Game</button>
    </div>;
};