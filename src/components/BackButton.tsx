import { useNavigate } from 'react-router-dom';
import { TexturedSurface } from './TexturedSurface';

export const BackButton = () => {
    const navigate = useNavigate();

    return (
        <TexturedSurface
            as="button"
            seed={0.2}
            className="btn-icon btn-icon--left"
            onClick={() => navigate('/dashboard')}
        >
            <span className="btn-icon__symbol">{'<'}</span>
        </TexturedSurface>
    );
};
