import json
gt=json.load(open('replay_decoded.json'))
logs=gt['logs']
delib=[json.loads(l) for l in open('delib.jsonl')]
by_ply={r['ply']:r for r in delib}

VAL={5:'6',6:'7',7:'8',8:'9',9:'10',10:'J',11:'Q',12:'K',13:'A'}
SUITSYM={0:'♠',1:'♥',2:'♣',3:'♦'}  # S H C D
def card(s,v):
    if v==-1: return {'r':'?','suit':s,'trump':False,'red':False,'hidden':True,'str':'??'}
    return {'r':VAL.get(v,'?'),'suit':s,'sym':SUITSYM[s],'trump':(s==0),'red':(s in (1,3)),
            'hidden':False,'str':VAL.get(v,'?')+SUITSYM[s]}

# reconstruct per-log table + hand counts
hc=[6,6]
table=[]  # list of {attack:card, defense:card|None}
out=[]
for i,l in enumerate(logs):
    t=l['t']; seat=l['seat']
    cards=[(x['p']['suit'],x['p']['value'],x['tg']) for x in l['cards']]
    action=None
    if t=='attack':
        for s,v,tg in cards: table.append({'attack':card(s,v),'defense':None})
        if seat is not None: hc[seat]-=len(cards)
        action={'kind':'attack','seat':seat,'cards':[card(s,v) for s,v,tg in cards]}
    elif t=='pass':
        for s,v,tg in cards: table.append({'attack':card(s,v),'defense':None})
        if seat is not None: hc[seat]-=len(cards)
        action={'kind':'pass','seat':seat,'cards':[card(s,v) for s,v,tg in cards]}
    elif t=='cover':
        for s,v,tg in cards:
            # find battle with matching uncovered attack
            for b in table:
                if b['defense'] is None and tg and b['attack']['suit']==tg['suit'] and b['attack']['r']==VAL.get(tg['value']):
                    b['defense']=card(s,v); break
        if seat is not None: hc[seat]-=len(cards)
        action={'kind':'cover','seat':seat,'cards':[card(s,v) for s,v,tg in cards],
                'targets':[card(tg['suit'],tg['value']) for s,v,tg in cards if tg]}
    elif t=='pickup':
        ncard=len(l['cards'])
        if seat is not None: hc[seat]+=ncard
        table=[]
        action={'kind':'pickup','seat':seat,'n':ncard,'cards':[card(x['p']['suit'],x['p']['value']) for x in l['cards']]}
    elif t=='good':
        action={'kind':'good','seat':seat}
    elif t=='discard':
        table=[]
        action={'kind':'discard','cards':[card(x['p']['suit'],x['p']['value']) for x in l['cards']]}
    elif t=='draw':
        n=len(l['cards']); 
        if seat is not None: hc[seat]+=n
        rev=[card(x['p']['suit'],x['p']['value']) for x in l['cards'] if x['p']['value']!=-1]
        action={'kind':'draw','seat':seat,'n':n,'reveal':rev}
    elif t=='defender_change':
        action={'kind':'defender_change','def':l['def']}
    elif t=='player_out':
        action={'kind':'player_out','seat':seat}
    elif t=='game_start':
        action={'kind':'game_start'}
    rec=None
    if i in by_ply:
        d=by_ply[i]
        # build candidate list with decoded, compute octogen's chosen label & recorded match
        cands=[]
        for c in d['candidates']:
            cands.append({'label':c['label'],'type':c['type'],'score':c['score'],'nsim':c['nsim'],
                          'alive':c['alive'],'verdict':c['verdict'],'forced_loss':c['forced_loss'],'chosen':c['chosen'],
                          'cards':[card(cc_s,cc_v) for (cc_s,cc_v) in [ _decode(tok) for tok in c['cards']]] if False else None})
        rec={'ply':i,'hand':d['hand'],'table':d['table'],'opp_counts':d['opp_counts'],
             'deck':d['deck'],'solver':d['solver'],'candidates':d['candidates'],'chosen':d['chosen']}
    out.append({'i':i,'t':t,'seat':seat,'def':l.get('def'),'action':action,
                'table':[{'a':b['attack'],'d':b['defense']} for b in table],
                'hc':list(hc),'decision':rec})

data={'meta':{'players':2,'trump':0,'trumpSym':SUITSYM[0],'seed':'da645ff515777b2c47d1c59937c7dbd637372ef1f2e440cf9867ea9cd2327d5f',
               'firstAttacker':1,'fool':0,'winner':1,'flip':card(0,5)},
      'flagged':[29,31,32,116],
      'logs':out}
json.dump(data,open('page_data.json','w'))
print('logs',len(out),'decisions',sum(1 for o in out if o['decision']))
