import { CardBack } from "./CardBack";
import { CardFace } from "./CardFace";
import { useServer } from "../../contexts/ServerContext";
import { useAnimation } from "../../contexts/AnimationContext";
import { SuitIcon } from "../SovietIcon";
import { rulesOf, type TableView } from "../../state/view";

export const DeckAndFlipped = () => {
    const game = useServer().view as TableView;
    const { inFlightFromDeck, inFlightToFlipped, trumpFlight } = useAnimation();

    // What the stock shows is the kernel's (client_view_rules): cards mid-flight
    // FROM the deck pile have left the visible pile, and cards mid-flight TO the
    // flipped slot are still in the deck system, so they count toward the badge
    // total. The flipped trump on its way to a hand has left its slot and the
    // count, so its face is not drawn here while it flies.
    //
    // THE PLACES A FLIGHT LEAVES FROM ARE ON SCREEN FOR AS LONG AS IT FLIES, and
    // that is the kernel's to say too: the stock's place outlives its last cards
    // while they are in the air (show_deck_spot), and the trump's slot outlives
    // the trump while it is (show_flipped_slot), so the overlay's
    // findElementByLocation('deck') and ('flipped') always find the place a
    // card is leaving. The slot gives way to the SuitIcon at end-game.
    const rules = rulesOf(game, inFlightFromDeck, inFlightToFlipped, trumpFlight !== null);
    const displayedDeckLength = rules.deckPile;
    const badgeTotal = rules.deckBadge;
    const showDeckPile = rules.showDeckPile;
    const showDeckSpot = rules.showDeckSpot;
    const showFlippedSlot = rules.showFlippedSlot;
    const showSuitIcon = rules.showTrumpIcon;

    // paddingTop = (height - CardBack height) / 2 = (240 - 70) / 2 — pins the
    // deck pile to the spot it would occupy when centered alone, so the deck
    // doesn't shift when the flipped card lands or when the slot is reserved.
    return <div style={{ display: 'flex', position: 'absolute', top: '0px', left: '0px', flexDirection: 'column', alignItems: 'center', height: '240px', width: '100px', paddingTop: '85px' }}>
        {showDeckSpot && (
            <div style={{ position: 'relative' }} data-location="deck">
                {/* An empty pile is the card's own footprint and no card. */}
                <CardBack deckSize={displayedDeckLength} />
                {showDeckPile && <p style={{
                    position: 'absolute',
                    top: '50%',
                    left: '50%',
                    transform: 'translate(-50%, -50%)',
                    margin: 0,
                    color: 'var(--color-text-primary)',
                    fontSize: '16px',
                    fontWeight: 'bold',
                    textShadow: '1px 1px 2px rgba(0,0,0,0.8)',
                    pointerEvents: 'none',
                    zIndex: 1003
                }}>
                    {badgeTotal}
                </p>}
            </div>
        )}
        {showFlippedSlot && (
            <div
                data-location="flipped"
                style={{
                    marginTop: showDeckSpot ? '-30px' : '0px',
                    width: '50px',
                    height: '70px',
                    zIndex: 0,
                }}
            >
                {rules.showFlippedCard && <CardFace card={game.flipped} owner="flipped" />}
            </div>
        )}
        {/* Trump indicator appears when deck and flipped card are gone */}
        {showSuitIcon && (
            <div style={{
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'center'
            }}>
                <SuitIcon suit={game.powerSuit} size={64} />
            </div>
        )}
    </div>
};