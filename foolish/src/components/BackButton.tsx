import { useRouter } from 'next/navigation';
import { TexturedSurface } from './TexturedSurface';

export const BackButton = () => {
    const router = useRouter();

    return (
        <TexturedSurface
            as="button"
            seed={0.2}
            className="btn-icon btn-icon--left"
            onClick={() => router.push('/dashboard')}
        >
            <span className="btn-icon__symbol">{'<'}</span>
        </TexturedSurface>
    );
};
