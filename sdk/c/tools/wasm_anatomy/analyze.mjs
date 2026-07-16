// WASM structural analyzer + disassembler for the foolish cnitro modules.
// Parses the STRIPPED shipped binary for exact structure/sizes, overlays
// function names from a name-preserving companion build (code section is
// byte-identical), disassembles every function body with a self-contained
// decoder (no objdump quirks), and emits one compact JSON blob per module.
import fs from 'node:fs';

const SECTION_NAMES = { 0:'custom',1:'type',2:'import',3:'function',4:'table',5:'memory',6:'global',7:'export',8:'start',9:'elem',10:'code',11:'data',12:'datacount' };
const KIND_NAMES = { 0:'func',1:'table',2:'mem',3:'global' };
const VALTYPE = { 0x7f:'i32',0x7e:'i64',0x7d:'f32',0x7c:'f64',0x70:'funcref',0x6f:'externref' };

class Reader {
  constructor(buf, pos=0){ this.b=buf; this.p=pos; }
  u8(){ return this.b[this.p++]; }
  vu(){ let r=0,s=0,byte; do{ byte=this.b[this.p++]; r+=(byte&0x7f)*2**s; s+=7; }while(byte&0x80); return r; }
  vs(){ let r=0,s=0,byte; do{ byte=this.b[this.p++]; r|=(byte&0x7f)<<s; s+=7; }while(byte&0x80); if(s<32&&(byte&0x40)) r|=(-1<<s); return r; }
  name(){ const n=this.vu(); const s=Buffer.from(this.b.slice(this.p,this.p+n)).toString('utf8'); this.p+=n; return s; }
  bytes(n){ const s=this.b.slice(this.p,this.p+n); this.p+=n; return s; }
}
function valtypeVec(r){ const n=r.vu(),out=[]; for(let i=0;i<n;i++) out.push(VALTYPE[r.u8()]||'?'); return out; }
function readLimits(r){ const f=r.u8(); const min=r.vu(); const max=(f&1)?r.vu():null; return {min,max,shared:!!(f&2)}; }
function readVS64(r){ let res=0n,shift=0n,byte; do{ byte=BigInt(r.b[r.p++]); res|=(byte&0x7fn)<<shift; shift+=7n; }while(byte&0x80n); if(byte&0x40n) res|=(~0n)<<shift; return res; }

function parse(buf){
  const r=new Reader(buf); r.bytes(4); const version=r.bytes(4).readUInt32LE(0);
  const out={ total:buf.length, version, sections:[], types:[], imports:[], functionTypeIdx:[], table:null, memory:null, globals:[], exports:[], elem:null, codeFuncs:[], data:[], customs:[], numImportedFuncs:0 };
  while(r.p<buf.length){
    const id=r.u8(); const size=r.vu(); const contentStart=r.p; const name=SECTION_NAMES[id]??`id${id}`;
    const sec={ id, name, size, start:contentStart }; out.sections.push(sec);
    const endp=contentStart+size; const sr=new Reader(buf, contentStart);
    try{
      if(id===1){ const n=sr.vu(); for(let i=0;i<n;i++){ sr.u8(); const params=valtypeVec(sr); const results=valtypeVec(sr); out.types.push({params,results}); } }
      else if(id===2){ const n=sr.vu(); for(let i=0;i<n;i++){ const mod=sr.name(),field=sr.name(),kind=sr.u8(); const imp={module:mod,field,kind:KIND_NAMES[kind]}; if(kind===0){ imp.typeIndex=sr.vu(); out.numImportedFuncs++; } else if(kind===1){ imp.et=VALTYPE[sr.u8()]; imp.limits=readLimits(sr); } else if(kind===2){ imp.limits=readLimits(sr); } else if(kind===3){ imp.valtype=VALTYPE[sr.u8()]; imp.mut=sr.u8(); } out.imports.push(imp); } }
      else if(id===3){ const n=sr.vu(); for(let i=0;i<n;i++) out.functionTypeIdx.push(sr.vu()); }
      else if(id===4){ const n=sr.vu(); const tabs=[]; for(let i=0;i<n;i++){ const et=VALTYPE[sr.u8()]; tabs.push({et,...readLimits(sr)}); } out.table=tabs; }
      else if(id===5){ const n=sr.vu(); const mems=[]; for(let i=0;i<n;i++) mems.push(readLimits(sr)); out.memory=mems; }
      else if(id===6){ const n=sr.vu(); for(let i=0;i<n;i++){ const vt=VALTYPE[sr.u8()]; const mut=sr.u8(); let initConst=null; const op=sr.u8(); if(op===0x41) initConst=sr.vs(); else if(op===0x42) initConst=Number(readVS64(sr)); let g=0; while(sr.b[sr.p]!==0x0b && g++<64) sr.p++; sr.u8(); out.globals.push({index:i,valtype:vt,mut:!!mut,initConst}); } }
      else if(id===7){ const n=sr.vu(); for(let i=0;i<n;i++){ const nm=sr.name(),kind=sr.u8(),idx=sr.vu(); out.exports.push({name:nm,kind:KIND_NAMES[kind],index:idx}); } }
      else if(id===9){ out.elem={size}; }
      else if(id===10){ const n=sr.vu(); for(let i=0;i<n;i++){ const bodySize=sr.vu(); const bodyStart=sr.p; const lr=new Reader(buf,bodyStart); const nd=lr.vu(); const locals=[]; let localCount=0; for(let j=0;j<nd;j++){ const c=lr.vu(); const t=VALTYPE[lr.u8()]||'?'; locals.push({count:c,type:t}); localCount+=c; } out.codeFuncs.push({ index: out.numImportedFuncs+i, bodySize, bodyStart, instrStart: lr.p, bodyEnd: bodyStart+bodySize, locals, localCount }); sr.p=bodyStart+bodySize; } }
      else if(id===11){ const n=sr.vu(); for(let i=0;i<n;i++){ const flags=sr.vu(); let memOffset=0; if(flags===0||flags===2){ if(flags===2) sr.vu(); const op=sr.u8(); if(op===0x41) memOffset=sr.vs(); let g=0; while(sr.b[sr.p]!==0x0b && g++<64) sr.p++; sr.u8(); } const len=sr.vu(); const b=sr.bytes(len); out.data.push({memOffset,size:len,allBytes:Buffer.from(b)}); } }
      else if(id===0){ const cr=new Reader(buf,contentStart); const cname=cr.name(); sec.customName=cname; out.customs.push({name:cname,size,contentStart:cr.p,contentEnd:endp}); }
    }catch(e){ sec.parseError=String(e); }
    r.p=endp;
  }
  return out;
}

function parseNameSection(buf, meta){
  const custom=meta.customs.find(c=>c.name==='name'); if(!custom) return {};
  const r=new Reader(buf, custom.contentStart); const names={};
  while(r.p<custom.contentEnd){ const subId=r.u8(); const subSize=r.vu(); const subEnd=r.p+subSize;
    if(subId===1){ const cnt=r.vu(); for(let i=0;i<cnt;i++){ const idx=r.vu(); names[idx]=r.name(); } } r.p=subEnd; }
  return names;
}

// ---- disassembler ----------------------------------------------------------
// Opcode table for the MVP + sign-extension + bulk-memory space this toolchain
// emits (verified: no SIMD/0xfd). imm codes drive operand decoding.
const OPS = {
  0x00:['unreachable',''],0x01:['nop',''],0x02:['block','bt'],0x03:['loop','bt'],0x04:['if','bt'],
  0x05:['else',''],0x0b:['end',''],0x0c:['br','u'],0x0d:['br_if','u'],0x0e:['br_table','table'],
  0x0f:['return',''],0x10:['call','call'],0x11:['call_indirect','callind'],
  0x1a:['drop',''],0x1b:['select',''],0x1c:['select','seltypes'],
  0x20:['local.get','u'],0x21:['local.set','u'],0x22:['local.tee','u'],0x23:['global.get','g'],0x24:['global.set','g'],
  0x25:['table.get','u'],0x26:['table.set','u'],
  0x28:['i32.load','mem'],0x29:['i64.load','mem'],0x2a:['f32.load','mem'],0x2b:['f64.load','mem'],
  0x2c:['i32.load8_s','mem'],0x2d:['i32.load8_u','mem'],0x2e:['i32.load16_s','mem'],0x2f:['i32.load16_u','mem'],
  0x30:['i64.load8_s','mem'],0x31:['i64.load8_u','mem'],0x32:['i64.load16_s','mem'],0x33:['i64.load16_u','mem'],
  0x34:['i64.load32_s','mem'],0x35:['i64.load32_u','mem'],
  0x36:['i32.store','mem'],0x37:['i64.store','mem'],0x38:['f32.store','mem'],0x39:['f64.store','mem'],
  0x3a:['i32.store8','mem'],0x3b:['i32.store16','mem'],0x3c:['i64.store8','mem'],0x3d:['i64.store16','mem'],0x3e:['i64.store32','mem'],
  0x3f:['memory.size','memidx'],0x40:['memory.grow','memidx'],
  0x41:['i32.const','s'],0x42:['i64.const','s64'],0x43:['f32.const','f32'],0x44:['f64.const','f64'],
  0xd0:['ref.null','reftype'],0xd1:['ref.is_null',''],0xd2:['ref.func','u'],
};
// numeric/comparison/conversion opcodes (no immediates)
const NOIMM = {
  0x45:'i32.eqz',0x46:'i32.eq',0x47:'i32.ne',0x48:'i32.lt_s',0x49:'i32.lt_u',0x4a:'i32.gt_s',0x4b:'i32.gt_u',0x4c:'i32.le_s',0x4d:'i32.le_u',0x4e:'i32.ge_s',0x4f:'i32.ge_u',
  0x50:'i64.eqz',0x51:'i64.eq',0x52:'i64.ne',0x53:'i64.lt_s',0x54:'i64.lt_u',0x55:'i64.gt_s',0x56:'i64.gt_u',0x57:'i64.le_s',0x58:'i64.le_u',0x59:'i64.ge_s',0x5a:'i64.ge_u',
  0x5b:'f32.eq',0x5c:'f32.ne',0x5d:'f32.lt',0x5e:'f32.gt',0x5f:'f32.le',0x60:'f32.ge',
  0x61:'f64.eq',0x62:'f64.ne',0x63:'f64.lt',0x64:'f64.gt',0x65:'f64.le',0x66:'f64.ge',
  0x67:'i32.clz',0x68:'i32.ctz',0x69:'i32.popcnt',0x6a:'i32.add',0x6b:'i32.sub',0x6c:'i32.mul',0x6d:'i32.div_s',0x6e:'i32.div_u',0x6f:'i32.rem_s',0x70:'i32.rem_u',0x71:'i32.and',0x72:'i32.or',0x73:'i32.xor',0x74:'i32.shl',0x75:'i32.shr_s',0x76:'i32.shr_u',0x77:'i32.rotl',0x78:'i32.rotr',
  0x79:'i64.clz',0x7a:'i64.ctz',0x7b:'i64.popcnt',0x7c:'i64.add',0x7d:'i64.sub',0x7e:'i64.mul',0x7f:'i64.div_s',0x80:'i64.div_u',0x81:'i64.rem_s',0x82:'i64.rem_u',0x83:'i64.and',0x84:'i64.or',0x85:'i64.xor',0x86:'i64.shl',0x87:'i64.shr_s',0x88:'i64.shr_u',0x89:'i64.rotl',0x8a:'i64.rotr',
  0x8b:'f32.abs',0x8c:'f32.neg',0x8d:'f32.ceil',0x8e:'f32.floor',0x8f:'f32.trunc',0x90:'f32.nearest',0x91:'f32.sqrt',0x92:'f32.add',0x93:'f32.sub',0x94:'f32.mul',0x95:'f32.div',0x96:'f32.min',0x97:'f32.max',0x98:'f32.copysign',
  0x99:'f64.abs',0x9a:'f64.neg',0x9b:'f64.ceil',0x9c:'f64.floor',0x9d:'f64.trunc',0x9e:'f64.nearest',0x9f:'f64.sqrt',0xa0:'f64.add',0xa1:'f64.sub',0xa2:'f64.mul',0xa3:'f64.div',0xa4:'f64.min',0xa5:'f64.max',0xa6:'f64.copysign',
  0xa7:'i32.wrap_i64',0xa8:'i32.trunc_f32_s',0xa9:'i32.trunc_f32_u',0xaa:'i32.trunc_f64_s',0xab:'i32.trunc_f64_u',0xac:'i64.extend_i32_s',0xad:'i64.extend_i32_u',0xae:'i64.trunc_f32_s',0xaf:'i64.trunc_f32_u',0xb0:'i64.trunc_f64_s',0xb1:'i64.trunc_f64_u',
  0xb2:'f32.convert_i32_s',0xb3:'f32.convert_i32_u',0xb4:'f32.convert_i64_s',0xb5:'f32.convert_i64_u',0xb6:'f32.demote_f64',0xb7:'f64.convert_i32_s',0xb8:'f64.convert_i32_u',0xb9:'f64.convert_i64_s',0xba:'f64.convert_i64_u',0xbb:'f64.promote_f32',
  0xbc:'i32.reinterpret_f32',0xbd:'i64.reinterpret_f64',0xbe:'f32.reinterpret_i32',0xbf:'f64.reinterpret_i64',
  0xc0:'i32.extend8_s',0xc1:'i32.extend16_s',0xc2:'i64.extend8_s',0xc3:'i64.extend16_s',0xc4:'i64.extend32_s',
};
const FC_OPS = { 0:'i32.trunc_sat_f32_s',1:'i32.trunc_sat_f32_u',2:'i32.trunc_sat_f64_s',3:'i32.trunc_sat_f64_u',4:'i64.trunc_sat_f32_s',5:'i64.trunc_sat_f32_u',6:'i64.trunc_sat_f64_s',7:'i64.trunc_sat_f64_u',8:'memory.init',9:'data.drop',10:'memory.copy',11:'memory.fill' };
const BT = { 0x40:'', 0x7f:'(result i32)',0x7e:'(result i64)',0x7d:'(result f32)',0x7c:'(result f64)' };

function readBlockType(r){
  // one byte 0x40 (empty) / valtype, OR a signed LEB s33 type index (>=0)
  const b=r.b[r.p];
  if(b===0x40){ r.p++; return ''; }
  if(VALTYPE[b] && (b&0x80)===0){ r.p++; return `(result ${VALTYPE[b]})`; }
  const t=r.vs(); return t>=0?`(type ${t})`:'';
}
function decodeFunc(buf, cf, ctx){
  const r=new Reader(buf, cf.instrStart); const end=cf.bodyEnd; const insns=[]; let depth=0;
  while(r.p<end){
    const start=r.p; const op=r.u8();
    let mn, operands='', comment='';
    // an `end`/`else` closes the current scope: dedent before emitting
    if(op===0x0b || op===0x05) depth=Math.max(0, depth-1);
    const emitDepth=depth;
    if(op in NOIMM){ mn=NOIMM[op]; }
    else if(op===0xfc){ const sub=r.vu(); mn=FC_OPS[sub]||`0xfc:${sub}`; if(sub===8){ r.vu(); r.u8(); } else if(sub===9){ r.vu(); } else if(sub===10){ r.u8(); r.u8(); } else if(sub===11){ r.u8(); } }
    else if(OPS[op]){ const [nm,imm]=OPS[op]; mn=nm;
      if(imm==='bt'){ operands=readBlockType(r); }
      else if(imm==='u'){ operands=String(r.vu()); }
      else if(imm==='g'){ const gi=r.vu(); operands=String(gi); const gn=ctx.globalName(gi); if(gn) comment=gn; }
      else if(imm==='call'){ const fi=r.vu(); operands=String(fi); const nm2=ctx.funcName(fi); if(nm2) comment='→ '+nm2; }
      else if(imm==='callind'){ const ty=r.vu(); r.vu(); operands=`(type ${ty})`; comment='indirect call'; }
      else if(imm==='table'){ const cnt=r.vu(); const targets=[]; for(let i=0;i<cnt;i++) targets.push(r.vu()); const def=r.vu(); operands=`${targets.join(' ')} ${def}`.trim(); comment=`${cnt} branch targets, default ${def}`; }
      else if(imm==='mem'){ const a=r.vu(); const o=r.vu(); operands = o? `offset=${o}${a?` align=${1<<a}`:''}` : (a? `align=${1<<a}`:''); }
      else if(imm==='memidx'){ r.u8(); }
      else if(imm==='s'){ const v=r.vs(); operands=String(v); const region=ctx.addrRegion(v); if(region) comment=region; }
      else if(imm==='s64'){ operands=readVS64(r).toString(); }
      else if(imm==='f32'){ operands=r.bytes(4).readFloatLE(0).toString(); }
      else if(imm==='f64'){ operands=r.bytes(8).readDoubleLE(0).toString(); }
      else if(imm==='reftype'){ operands=VALTYPE[r.u8()]||'?'; }
      else if(imm==='seltypes'){ const n=r.vu(); const ts=[]; for(let i=0;i<n;i++) ts.push(VALTYPE[r.u8()]); operands=ts.join(' '); }
    }
    else { mn=`0x${op.toString(16).padStart(2,'0')}`; comment='unknown opcode'; }
    // block/loop/if open a new scope: indent following instructions.
    // `else` re-opens for the else-arm.
    if(op===0x02||op===0x03||op===0x04||op===0x05) depth++;
    const rawBytes=[]; for(let i=start;i<r.p;i++) rawBytes.push(buf[i].toString(16).padStart(2,'0'));
    insns.push({ o:(start-cf.instrStart), b:rawBytes.join(''), t:mn, a:operands, c:comment, d:emitDepth });
  }
  return insns;
}

function analyze(key, strippedPath, namedPath){
  const stripped=fs.readFileSync(strippedPath); const meta=parse(stripped);
  // Names come from a name-preserving companion build if one is given (cnitro:
  // the shipped binary is --strip-all but the companion's CODE section is
  // byte-identical), otherwise from the wasm's OWN name section, otherwise
  // functions stay func[N]. This is what lets the tool run on any wasm.
  let idxName={};
  if(namedPath && namedPath!==strippedPath && fs.existsSync(namedPath)){
    const named=fs.readFileSync(namedPath); idxName=parseNameSection(named, parse(named));
  } else {
    idxName=parseNameSection(stripped, meta);
  }

  // memory regions from pointer-export constants — resolved after we know exports+consts
  const ptrConsts={};

  const funcNameOf=(i)=> idxName[i] || `func[${i}]`;
  const globalNameOf=(i)=>{ const g=meta.globals[i]; if(!g) return null; if(i===0) return '__stack_pointer'; return null; };

  // Build a memory-region resolver for i32.const annotations (filled below).
  let regions=[];
  const addrRegion=(v)=>{ if(v<=0) return ''; for(const rg of regions){ if(v>=rg.start && v<rg.end) return rg.label; } return ''; };
  const ctx={ funcName:funcNameOf, globalName:globalNameOf, addrRegion };

  const funcs=meta.codeFuncs.map((cf,i)=>{
    const name=funcNameOf(cf.index); const typeIdx=meta.functionTypeIdx[i]; const t=meta.types[typeIdx]||{params:[],results:[]};
    const ins=decodeFunc(stripped, cf, ctx);
    return { index:cf.index, name, size:cf.bodySize, sig:t, locals:cf.locals, localCount:cf.localCount, ins };
  });

  // opcode histogram
  const opcodes={};
  for(const f of funcs) for(const ins of f.ins){ opcodes[ins.t]=(opcodes[ins.t]||0)+1; }

  // pointer-export constants (tiny getter bodies)
  for(const ex of meta.exports.filter(e=>e.kind==='func')){
    const f=funcs.find(ff=>ff.index===ex.index); if(!f) continue;
    for(const ins of f.ins){ if(ins.t==='i32.const'){ ptrConsts[ex.name]=parseInt(ins.a,10); break; } }
  }
  return { key, meta, funcs, opcodes, ptrConsts };
}

// ---- run --------------------------------------------------------------------
// Config-driven: `node analyze.mjs <outdir> <config.json>`
//   config.modules = [{ key, wasm, named? }]
// (a bare `<outdir> <buildDir>` invocation still works for the cnitro layout.)
const OUT=process.argv[2]||'.';
let modules;
const arg3=process.argv[3];
if(arg3 && arg3.endsWith('.json') && fs.existsSync(arg3)){
  const cfg=JSON.parse(fs.readFileSync(arg3,'utf8'));
  modules=cfg.modules.map(m=>({key:m.key, stripped:m.wasm, named:m.named||m.wasm}));
} else {
  const B=arg3||'/home/user/foolish/sdk/c/build';
  modules=[
    {key:'rules', stripped:`${B}/rules.wasm`,  named:`${B}/named/rules.named.wasm`},
    {key:'guards',stripped:`${B}/guards.wasm`, named:`${B}/named/guards.named.wasm`},
    {key:'bots',  stripped:`${B}/bots.wasm`,   named:`${B}/named/bots.named.wasm`},
  ];
}
for(const m of modules){
  const res=analyze(m.key,m.stripped,m.named);
  const dataOut=res.meta.data.map(d=>({memOffset:d.memOffset,size:d.size,b64:d.allBytes.toString('base64')}));
  const json={ key:res.key, total:res.meta.total, version:res.meta.version,
    sections:res.meta.sections.map(s=>({id:s.id,name:s.name,size:s.size,customName:s.customName})),
    types:res.meta.types, imports:res.meta.imports, exports:res.meta.exports,
    table:res.meta.table, memory:res.meta.memory, globals:res.meta.globals,
    numImportedFuncs:res.meta.numImportedFuncs, funcs:res.funcs, opcodes:res.opcodes,
    ptrConsts:res.ptrConsts, data:dataOut, customs:res.meta.customs.map(c=>({name:c.name,size:c.size})) };
  const p=`${OUT}/${m.key}.json`; fs.writeFileSync(p, JSON.stringify(json));
  const insTotal=res.funcs.reduce((a,f)=>a+f.ins.length,0);
  console.log(`${m.key}: total=${json.total} funcs=${json.funcs.length} insns=${insTotal} opcodes=${Object.keys(json.opcodes).length} sizeJSON=${(fs.statSync(p).size/1e6).toFixed(2)}MB`);
}
