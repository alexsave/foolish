import { Card } from "../../common/types";
import { VALUE_MAP, SUIT_MAP } from "../../utils/cards";

export const CardFace = ({ card, onClick }: { card: Card, onClick?: () => void }) => {
    return (
        <div onClick={onClick} style={{ backgroundColor: 'white', width: '40px', height: '70px', borderRadius: '5px', border: '2px solid black', display: 'flex', justifyContent: 'center', alignItems: 'center' }}>
            <p style={{ margin: 0, fontSize: '18px', textAlign: 'center' }}>
                {VALUE_MAP[card.value]}
                <br />
                {SUIT_MAP[card.suit]}
            </p>
        </div>
    )
}