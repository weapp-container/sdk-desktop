// Authored lifecycle fixture, independent of compiler and device acceptance.
import { createHash } from 'node:crypto';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
const out=resolve(process.argv[2]);mkdirSync(resolve(out,'blobs/sha256'),{recursive:true});
const canonical=value=>Array.isArray(value)?`[${value.map(canonical).join(',')}]`:value!==null&&typeof value==='object'?`{${Object.keys(value).sort().map(key=>`${JSON.stringify(key)}:${canonical(value[key])}`).join(',')}}`:JSON.stringify(value);
const hash=value=>createHash('sha256').update(value).digest('hex');
const blob=value=>{const bytes=typeof value==='string'?value:canonical(value),sha=hash(bytes);writeFileSync(resolve(out,'blobs/sha256',sha),bytes);return sha;};
const page=`Page({onLoad(query){weappContainer.postMessage({kind:'start',query,identity:weappContainer.identity.getState()});weappContainer.onMessage(message=>{if(message.op==='login')wx.login({success:value=>weappContainer.postMessage({kind:'login',value}),fail:error=>weappContainer.postMessage({kind:'login-failure',error})});});}});`;
const manifest={schemaVersion:1,protocolVersion:'1.0',compiler:{name:'weapp-vite',commit:'authored-desktop-account-lifecycle-fixture',platform:'weapp'},app:{appJson:'app.json',entryModule:'app.js'},routes:[{id:'route:pages/home/index',path:'pages/home/index',kind:'page',config:'pages/home/index.json',entryModule:'pages/home/index.js'}],modules:[{id:'module:app.js',format:'commonjs',scope:'app',sha256:blob('App({});')},{id:'module:app.json',format:'json',scope:'app',sha256:blob({pages:['pages/home/index']})},{id:'module:pages/home/index.js',format:'commonjs',scope:'page',sha256:blob(page)},{id:'module:pages/home/index.json',format:'json',scope:'page',sha256:blob({})}],templates:[{id:'template:pages/home/index.wxml',wxmlAst:`sha256:${blob({version:1,nodes:[{kind:'element',tag:'view',attrs:{},bindings:[],children:[{kind:'text',value:'Account lifecycle',bindings:[]}]}]})}`,bindings:[]}],styles:[],components:[],assets:[],capabilities:{'wx.login':'partial','weappContainer.identity.getState':'partial'},integrity:{algorithm:'sha256',root:''}};
manifest.integrity.root=hash(canonical(manifest));writeFileSync(resolve(out,'manifest.json'),canonical(manifest));
console.log(manifest.integrity.root);
