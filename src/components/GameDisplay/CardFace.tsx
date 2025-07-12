import { SUIT_MAP, VALUE_MAP } from "../../utils/cards";
import { Card } from "../../common/types";

export const CardFace = ({ card, onClick, style = {}, ...props }: { card: Card, onClick?: () => void, style?: React.CSSProperties } & React.HTMLAttributes<HTMLDivElement>) => {
    const defaultStyle: React.CSSProperties = {
        backgroundColor: 'white',
        width: '50px',
        height: '70px',
        borderRadius: '5px',
        border: '2px solid black',
        display: 'flex',
        justifyContent: 'center',
        alignItems: 'center',
        cursor: onClick ? 'pointer' : 'default',
        userSelect: 'none',
        pointerEvents: onClick ? 'auto' : 'none' // Don't block pointer events unless there's an onClick
    }

    return (
        <div onClick={onClick} style={{ ...defaultStyle, ...style }} {...props}>
            <p style={{
                pointerEvents: 'none',
                userSelect: 'none',
                textAlign: 'center',
                fontSize: '20px',
                margin: '1px'
            }}>
                {VALUE_MAP[card.value]}
                <br />
                {SUIT_MAP[card.suit]}
            </p>
        </div>
    )
}