import { Card, PersonalGame } from '../common/common';
import React, { useState, useEffect } from 'react';
import { useServer } from '../contexts/ServerContext';
import { useParams } from 'react-router-dom';

const SUIT_MAP: Record<number, string> = {
  // emojis
  0: '♠️',
  1: '♥️',
  2: '♣️',
  3: '♦️',
}

const VALUE_MAP: Record<number, string> = {
  1: '2',
  2: '3',
  3: '4',
  4: '5',
  5: '6',
  6: '7',
  7: '8',
  8: '9',
  9: '10',
  10: 'J',
  11: 'Q',
  12: 'K',
  13: 'A',
}

// Ok let's actually look at the game state to see if we are defending and modify options

export const GameDisplay = () => {
  const { game, player_id, attack, game_id, pass, pickup, setGameIdFromUrl, loadGame } = useServer();
  const { game_id: urlGameId } = useParams();
  const state = game as PersonalGame;
  const [selectedCards, setSelectedCards] = useState<Card[]>([]);

  const [coverMap, setCoverMap] = useState<Map<Card, Card>>(new Map());

  useEffect(() => {
    if (urlGameId && urlGameId !== game_id) {
      setGameIdFromUrl(urlGameId);
      loadGame(urlGameId);
    }
  }, [urlGameId, game_id, setGameIdFromUrl, loadGame]);

  if (!state || !state.players || !state.players.length) {
    return <div>Loading...</div>;
  }

  const self_index = state.players.findIndex((player) => player.id === player_id);

  const isDefending = state.currentlyAttacked === self_index;

  // a set


  const CardDisplay = ({card}: {card: Card}) => {
    return (
      <div style={{ backgroundColor: 'white', width: '40px', height: '70px', borderRadius: '5px', border: '1px solid black', display: 'flex', justifyContent: 'center', alignItems: 'center' }}>
        <p>{VALUE_MAP[card.value] + SUIT_MAP[card.suit]}</p>
      </div>
    )
  }

  const CardBack = () => {
    return (
      <div style={{ backgroundColor: 'black', width: '40px', height: '70px', borderRadius: '5px', border: '1px solid black', display: 'flex', justifyContent: 'center', alignItems: 'center' }}>
        <p>?</p>
      </div>
    )
  }

  return (
    <div style={{ backgroundColor: '#982621', width: '100%', height: '100vh'}}>
      <div style={{  display: 'flex',  flex: 1, justifyContent: 'center', alignItems: 'center' }}>
        <p>FOOLISH</p>
        <div style={{ display: 'flex', position: 'absolute', top: '0px', left: '0px', flexDirection: 'column', justifyContent: 'center', alignItems: 'center', height: '100px' }}>
          {state.flipped && <CardDisplay card={state.flipped} />}
          <CardBack />
          <p>{JSON.stringify(state.deck_length)}</p>
        </div>
        <div style={{ display: 'flex', flexDirection: 'column', position: 'absolute', bottom: '10px', left: '0px', right: '0px', justifyContent: 'end', alignItems: 'center', height: '200px' }}>
          {
            selectedCards.length > 0 && <div style={{ backgroundColor: 'rgba(0, 0, 0, 0.5)', zIndex: 999 }}>
              <p>{JSON.stringify(selectedCards)}</p>

              {
                isDefending ? (
                  <>
                    <button onClick={() => { 
                      console.log(selectedCards);
                      pass(selectedCards).then(() => {
                        setSelectedCards([]);
                      }).catch((e) => {
                        console.error(e.message);
                      })
                    }}>Pass</button>
                    <button onClick={() => { 
                      pickup().then(() => {
                        // add cards to hand???
                      }).catch((e) => {
                        console.error(e.message);
                      })
                    }}>Pickup</button>

                    <button onClick={() => { }}>Cover</button>
                  </>
                ) : (
                  <button onClick={() => attack(selectedCards).then(() => {
                    setSelectedCards([]);
                  }).catch((e) => {
                    console.error(e.message);
                  })}>Attack</button>
                )
              }

            </div>
          }
          <div style={{ display: 'flex', flexDirection: 'row', alignItems: 'center', justifyContent: 'center' }}>

            {
              state.self.hand.map((card) => {
                const style = selectedCards.includes(card) ? { border: '3px solid red' } : { border: '1px solid black' };
                return (
                  <div
                    key={'' + card.value + card.suit}
                    // oh boy we're going to have fun with zindex
                    style={{ ...style, zIndex: 1000, backgroundColor: 'white', width: '40px', height: '70px', borderRadius: '5px', display: 'flex', justifyContent: 'center', alignItems: 'center' }}
                    onClick={(e) => {
                      e.stopPropagation();
                      if (selectedCards.includes(card)) {
                        setSelectedCards([...selectedCards].filter(c => c !== card));
                      } else {
                        setSelectedCards([...selectedCards, card]);
                      }
                    }}
                  >
                    <p>{VALUE_MAP[card.value] + SUIT_MAP[card.suit]}</p>
                  </div>
                )
              })
            }
          </div>

        </div>

        <div style={{ position: 'absolute', display: 'flex', flexDirection: 'column', top: 0, width: '100%', bottom: 0, alignItems: 'center', justifyContent: 'center' }}>
          <div style={{ backgroundColor: 'black', height: '10px', width: '10px' }} />
          <div>
            {JSON.stringify(state.table)}
          </div>
          {
            state.table.map((battle, index) => {
              return <div key={battle.attack.value + ' ' +battle.attack.suit}style={{ display: 'flex', flexDirection: 'row', alignItems: 'center', justifyContent: 'center' }}>
                <CardDisplay card={battle.attack} />
                {battle.defense && <CardDisplay card={battle.defense} />}
              </div>
            })
          }
          {
            state.players.map((player, index) => {

              const visual_index = (index - self_index + state.players.length) % state.players.length;
              // array of 100 black squares
              //Array.from({length: state.players.length-1}).map((_, index) => {
              const radians = (2) * Math.PI * visual_index / (state.players.length)// + Math.PI / 4;
              const x = ((-1* Math.sin(radians) * 30) + 50) + '%';
              const y = ((Math.cos(radians) * 30) + 50) + '%';

              let color = 'black';
              if (index === state.currentlyAttacked) {
                color = 'red';
              } else if (index === state.firstAttacker) {
                color = 'orange';
              }

              return <div key={player.id} style={{ backgroundColor: color, height: '10px', width: '10px', position: 'absolute', top: y, left: x }}>
                <p>{player.name}</p>
                {player.hand_length && <p>{player.hand_length}</p>}
              </div>
            })
          }
        </div>
      </div>
    </div>
  );
};