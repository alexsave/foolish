import { useNavigate } from "react-router-dom";
import { TexturedSurface } from "./TexturedSurface";

export const Tutorial = () => { 
    const navigate = useNavigate();

    return (
        <div>
            <h1>Tutorial</h1>
            <p>Insert a cool tutorial here with a sample game and everything</p>
            <TexturedSurface
                as="button"
                seed={0.3}
                className="btn-wood btn-wood--md"
                onClick={() => navigate('/dashboard')}
            >
                <span className="btn-wood-text">Go to Game</span>
            </TexturedSurface>
        </div>
    );
};
