// Authored real-engine fixture. No account credentials or issuer secrets.
import { createHash } from 'node:crypto';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
const out=resolve(process.argv[2]);mkdirSync(resolve(out,'blobs/sha256'),{recursive:true});
const canonical=value=>Array.isArray(value)?`[${value.map(canonical).join(',')}]`:value!==null&&typeof value==='object'?`{${Object.keys(value).sort().map(key=>`${JSON.stringify(key)}:${canonical(value[key])}`).join(',')}}`:JSON.stringify(value);
const hash=value=>createHash('sha256').update(value).digest('hex');
const blob=value=>{const bytes=typeof value==='string'?value:canonical(value),sha=hash(bytes);writeFileSync(resolve(out,'blobs/sha256',sha),bytes);return sha;};
const page=`Page({onPhone(event){weappContainer.postMessage({kind:'phone-event',value:event.detail});},onLoad(query){
 const post=value=>weappContainer.postMessage(value);
 post({kind:'start',query,identity:weappContainer.identity.getState()});
 weappContainer.identity.onChange(value=>post({kind:'identity',value}));
 weappContainer.onMessage(message=>{
  if(message.op==='state'){post({kind:'state',value:weappContainer.identity.getState()});return;}
  const api={login:wx.login,check:wx.checkSession,profile:wx.getUserProfile,info:wx.getUserInfo,logout:weappContainer.identity.logout,payment:weappContainer.payment.request}[message.op];
  if(!api){post({kind:'missing',op:message.op});return;}
  const options=Object.assign({},message.payload,{success:value=>post({kind:message.op+'-callback',value}),fail:error=>post({kind:message.op+'-failure',error}),complete:()=>post({kind:message.op+'-complete'})});
  api(options).then(value=>post({kind:message.op+'-promise',value}),error=>post({kind:message.op+'-reject',error}));
 });
}});`;
const manifest={schemaVersion:1,protocolVersion:'1.0',compiler:{name:'weapp-vite',commit:'authored-desktop-host-services-fixture',platform:'weapp'},app:{appJson:'app.json',entryModule:'app.js'},routes:[{id:'route:pages/home/index',path:'pages/home/index',kind:'page',config:'pages/home/index.json',entryModule:'pages/home/index.js'}],modules:[{id:'module:app.js',format:'commonjs',scope:'app',sha256:blob('App({});')},{id:'module:app.json',format:'json',scope:'app',sha256:blob({pages:['pages/home/index']})},{id:'module:pages/home/index.js',format:'commonjs',scope:'page',sha256:blob(page)},{id:'module:pages/home/index.json',format:'json',scope:'page',sha256:blob({})}],templates:[{id:'template:pages/home/index.wxml',wxmlAst:`sha256:${blob({version:1,nodes:[{kind:'element',tag:'button',attrs:{id:'phone-button','open-type':'getPhoneNumber',bindgetphonenumber:'onPhone'},bindings:[],children:[{kind:'text',value:'Authorize phone',bindings:[]}]}]})}`,bindings:[]}],styles:[],components:[],assets:[],capabilities:Object.fromEntries(['wx.login','wx.checkSession','wx.getUserInfo','wx.getUserProfile','weappContainer.identity.getState','weappContainer.identity.onChange','weappContainer.identity.offChange','weappContainer.identity.authorizePhone','weappContainer.identity.logout','weappContainer.payment.request','component:button.open-type.getPhoneNumber','component:button.bindgetphonenumber'].map(name=>[name,'partial'])),integrity:{algorithm:'sha256',root:''}};
manifest.integrity.root=hash(canonical(manifest));writeFileSync(resolve(out,'manifest.json'),canonical(manifest));
console.log(manifest.integrity.root);
