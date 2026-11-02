// Actual reference HTTP/SQLite service + installed native transport + QuickJS.
// Native prompts are controlled test presenters; no OS UI/hardware/merchant claim.
import assert from 'node:assert/strict';
import { once } from 'node:events';
import { createServer } from 'node:http';
import { randomBytes } from 'node:crypto';
import { spawn } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { resolve, join } from 'node:path';
import { pathToFileURL } from 'node:url';
const [referenceRoot,binary,pack] = process.argv.slice(2).map(value=>resolve(value));
const { HostServices } = await import(pathToFileURL(join(referenceRoot,'src/service.mjs')));
const { createHostServicesHandler } = await import(pathToFileURL(join(referenceRoot,'src/server.mjs')));
const { referenceSignature } = await import(pathToFileURL(join(referenceRoot,'src/crypto.mjs')));
const secret=()=>randomBytes(32).toString('base64url');
const clientSecret=secret(),callbackSecret=secret(),control=secret();
const packRoot=JSON.parse(readFileSync(join(pack,'manifest.json'))).integrity.root;
const app={miniAppId:'desktop-services',hostAppId:'desktop-services-host',tenantId:'tenant',environment:'trial',clientId:'business-backend',clientSecret,
  trustedPackRoots:[packRoot],allowedPaymentChannels:['reference'],authorizationPolicies:{profile:{purpose:'Display the authorized profile',permissionVersion:'profile-v1'},phone:{purpose:'Associate the verified phone',permissionVersion:'phone-v1'}}};
const service=new HostServices({issuerSecret:secret(),identitySecret:secret(),applications:[app],payments:{reference:{callbackSecret}}});
const tokens={};
for(const [principalId,principalEpoch] of [['alice','1'],['bob','2'],['carol','3'],['dave','4']])tokens[principalId]=service.issueHostSession({hostAppId:app.hostAppId,tenantId:app.tenantId,environment:app.environment,principalId,principalEpoch,profile:{nickName:principalId},phone:'+8613800000000'}).hostAccessToken;
const selection=principal=>({miniAppId:app.miniAppId,packRoot,expectedPrincipalId:principal,expectedPrincipalEpoch:{alice:'1',bob:'2',carol:'3'}[principal]});
const sessions={};for(const principal of ['alice','bob'])sessions[principal]=service.exchangeLogin(app.clientId,clientSecret,service.loginCode(tokens[principal],selection(principal)).code).accessToken;
const orders={};for(const [name,principal] of [['confirmed','alice'],['pending','alice'],['foreign','bob']])orders[name]=await service.createOrder(sessions[principal],{channel:'reference',product:'app',amount:1,currency:'CNY',description:'Desktop controlled reference',idempotencyKey:name});
const handler=createHostServicesHandler(service);let redirectLeaks=0,loginDelay=0;const timers=new Set();
const server=createServer(async(request,response)=>{
  if(process.env.WEAPP_HOST_SERVICES_TEST_TRACE)console.error('native reference HTTP',request.url);
  const bearer=request.headers.authorization?.slice(7);
  if(bearer==='redirect-fixture'){response.writeHead(307,{Location:'/fixture/redirect-leak'});response.end();return;}
  if(request.url==='/fixture/redirect-leak'){redirectLeaks++;response.writeHead(500);response.end('{}');return;}
  if(bearer==='oversized-fixture'){response.writeHead(200,{'Content-Type':'application/json'});response.end(JSON.stringify({padding:'x'.repeat(262145)}));return;}
  if(bearer==='challenge-fixture'){response.writeHead(401,{'WWW-Authenticate':'Basic realm="native-fixture"'});response.end('{}');return;}
  if(request.url.startsWith('/fixture/')){
    if(bearer!==control){response.writeHead(403);response.end('{}');return;}
    let data='';for await(const chunk of request){data+=chunk;assert.ok(data.length<262144);}const body=JSON.parse(data||'{}');
    try {
      if(request.url==='/fixture/exchange'){
        const exchanged=service.exchangeLogin(app.clientId,clientSecret,body.code);
        assert.throws(()=>service.exchangeLogin(app.clientId,clientSecret,body.code),/expired|used|consumed/i);
        response.end(JSON.stringify({exchanged:!!exchanged.appUserId}));return;
      }
      if(request.url==='/fixture/confirm'){
        service.launchForHost(tokens.alice,body.orderId,selection('alice'));
        const order=Object.values(orders).find(order=>order.orderId===body.orderId);assert.ok(order);
        const raw=JSON.stringify({eventId:`desktop-${order.orderId}`,merchantOrderId:order.merchantOrderId,miniAppId:app.miniAppId,amount:1,currency:'CNY',transactionId:`ref-${order.orderId}`,status:'paid'});
        const timestamp=String(Math.floor(Date.now()/1000));const headers={'x-reference-timestamp':timestamp,'x-reference-signature':referenceSignature(callbackSecret,timestamp,raw)};
        service.notification('reference',headers,raw);service.notification('reference',headers,raw);
        response.end(JSON.stringify({confirmed:service.getOrder(sessions.alice,body.orderId).status==='paid'}));return;
      }
      if(request.url==='/fixture/delay'){loginDelay=300;response.end('{}');return;}
    }catch(error){response.writeHead(400);response.end(JSON.stringify({error:'fixture_assertion',message:error.message}));return;}
    response.writeHead(404);response.end('{}');return;
  }
  if(bearer==='slow-fixture'||bearer===tokens.dave&&request.url==='/v1/host/identity-state'||loginDelay&&request.url==='/v1/host/login-codes'){
    const timer=setTimeout(()=>{timers.delete(timer);handler(request,response);},bearer==='slow-fixture'?1000:bearer===tokens.dave?300:loginDelay);timers.add(timer);loginDelay=0;
  }else await handler(request,response);
});
const directory=mkdtempSync(join(tmpdir(),'weapp-desktop-host-services-'));
try {
  server.listen(0,'127.0.0.1');await once(server,'listening');
  const endpoint=`http://127.0.0.1:${server.address().port}`;
  const input=join(directory,'native-fixture.json');
  writeFileSync(input,JSON.stringify({endpoint,control,tokens,orders:Object.fromEntries(Object.entries(orders).map(([name,value])=>[name,value.orderId])),configuration:{application:{hostAppId:app.hostAppId,tenantId:app.tenantId,publisherId:'publisher',appId:app.miniAppId,environment:app.environment,version:'1.0.0',packRoot},account:{principalId:'alice',epoch:'1'}}}),{mode:0o600});
  const child=spawn(binary,[pack,input],{stdio:['ignore','inherit','inherit']});
  const [code,signal]=await once(child,'exit');assert.equal(signal,null);assert.equal(code,0);
  assert.equal(redirectLeaks,0,'Native transport forwarded a bearer across redirect');
  console.log('Actual reference service + native transport + QuickJS: passed; no native UI or merchant transaction was executed.');
} finally {
  for(const timer of timers)clearTimeout(timer);timers.clear();
  server.closeAllConnections();await new Promise((done,reject)=>server.close(error=>error?reject(error):done()));
  service.close();rmSync(directory,{recursive:true,force:true});
}
