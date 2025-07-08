import { PersonalGame } from "../../common/types";
import { CardBack } from "./CardBack";
import { CardFace } from "./CardFace";
import { SUIT_MAP } from "../../utils/cards";

export const DeckAndFlipped = ({ state }: { state: PersonalGame }) => {
    return <div style={{ display: 'flex', position: 'absolute', top: '0px', left: '0px', flexDirection: 'column', justifyContent: 'center', alignItems: 'center', height: '240px', width: '100px' }}>
        {state.deck_length > 0 && (
            <div style={{ position: 'relative' }}>
                <CardBack deckSize={state.deck_length} />
                <p style={{
                    position: 'absolute',
                    top: '50%',
                    left: '50%',
                    transform: 'translate(-50%, -50%)',
                    margin: 0,
                    color: 'white',
                    fontSize: '16px',
                    fontWeight: 'bold',
                    textShadow: '1px 1px 2px rgba(0,0,0,0.8)',
                    pointerEvents: 'none',
                    zIndex: 1003
                }}>
                    {state.deck_length + (state.flipped ? 1 : 0)}
                </p>
            </div>
        )}
        {state.flipped && (
            <div style={{ marginTop: state.deck_length > 0 ? '-30px' : '0px' }}>
                <CardFace card={state.flipped} />
            </div>
        )}
        {/* Trump indicator appears when deck and flipped card are gone */}
        {state.deck_length === 0 && !state.flipped && (
            <div style={{
                fontSize: '64px',
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'center'
            }}>
                <span>{SUIT_MAP[state.power_suit]}</span>
            </div>
        )}
    </div>
};