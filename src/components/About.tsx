import { TexturedSurface } from './TexturedSurface';
import { WoolBackgroundLayer } from './WoolBackgroundLayer';
import { LanguageSwitcher } from './LanguageSwitcher';
import { Text } from './Text';
import { Link } from 'react-router-dom';

export const About = () => {
    return (
        <div className="page page--centered page--with-padding" style={{ width: '100vw' }}>
            <WoolBackgroundLayer />
            
            <div className="content content--max-width content--centered content--gap-lg z-content">
                <h1 className="title title--page">
                    <Text id="about_foolish" />
                </h1>

                <div className="about-content">
                    <p>
                        <Text id="about_paragraph_1" />
                    </p>
                    <p>
                        <Text id="about_paragraph_2" />
                    </p>
                    <p>
                        <Text id="about_paragraph_3" />
                    </p>
                </div>

                <TexturedSurface
                    as={Link}
                    seed={0.5}
                    to="/"
                    className="btn-wood btn-wood--md"
                    style={{ textDecoration: 'none', display: 'inline-block' }}
                >
                    <span className="btn-wood-text">
                        <Text id="back_to_home" />
                    </span>
                </TexturedSurface>
            </div>
            
            <LanguageSwitcher />
        </div>
    );
};
