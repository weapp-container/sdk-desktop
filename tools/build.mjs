#!/usr/bin/env node
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { cpSync, existsSync, mkdirSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { copyPack, readNative, readNotices, requireNativeRoot, sourceProvenance, nativeIdentity } from './native-input.mjs';
const root=resolve(dirname(fileURLToPath(import.meta.url)),'..');
const args=process.argv.slice(2);
function option(name,fallback){const i=args.indexOf(name);if(i<0)return fallback;if(!args[i+1]||args[i+1].startsWith('--'))throw new Error(`${name} requires a value`);return args[i+1];}
const platform=option('--target',process.platform==='darwin'?'macos-universal':process.platform==='win32'?'windows-x64':'linux-x64');
const targets={
  'macos-universal':['aarch64-apple-darwin','x86_64-apple-darwin'],
  'macos-arm64':['aarch64-apple-darwin'],'macos-x64':['x86_64-apple-darwin'],
  'windows-x64':['x86_64-pc-windows-msvc'],'linux-x64':['x86_64-unknown-linux-gnu'],
}[platform];
if(!targets)throw new Error(`Unsupported desktop target ${platform}`);
if(platform.startsWith('macos')&&process.platform!=='darwin'||platform.startsWith('windows')&&process.platform!=='win32'||platform.startsWith('linux')&&process.platform!=='linux')throw new Error('Desktop packaging requires the target platform host');
const out=resolve(option('--out',resolve(root,'artifacts/desktop')));
const prefix=resolve(out,platform),build=resolve(out,`.build-${platform}`),consumer=resolve(out,`.consumer-${platform}`);
const nativeRoot=requireNativeRoot(args);
const licensesRoot=resolve(option('--licenses-root',resolve(nativeRoot,'../licenses')));
if(args.includes('--verify')&&!option('--pack'))throw new Error('--pack is required for consumer execution');
mkdirSync(prefix,{recursive:true});
function query(command,argv){const r=spawnSync(command,argv,{cwd:root,encoding:'utf8'});if(r.error||r.status!==0)throw new Error(`${command} failed: ${r.error?.message||r.stderr}`);return r.stdout.trim();}
const receipt={schemaVersion:1,sdkVersion:'0.1.0-preview.4',platform,...sourceProvenance(root),toolchain:{},startedAt:new Date().toISOString(),status:'building',native:[],checks:[],artifacts:[]};
const receiptPath=resolve(prefix,'package.json');
const validationDir=resolve(out,`validation-${platform}`);mkdirSync(validationDir,{recursive:true});
const commandEvidence=[];
const publicMessage=value=>[root,nativeRoot,licensesRoot,out].sort((a,b)=>b.length-a.length).reduce((text,path)=>text.replaceAll(path,'<local-build-directory>'),String(value));
function save(){
  const distribution={...receipt,checks:receipt.checks.map(check=>({...check,...(check.error?{error:publicMessage(check.error)}:{})})),...(receipt.error?{error:publicMessage(receipt.error)}:{})};
  writeFileSync(receiptPath,JSON.stringify(distribution,null,2)+'\n');
  writeFileSync(resolve(validationDir,'report.json'),JSON.stringify({...receipt,commands:commandEvidence},null,2)+'\n');
}

function run(cmd,argv,label){
  console.log(`\n${label}`);
  const r=spawnSync(cmd,argv,{cwd:root,encoding:'utf8',maxBuffer:32*1024*1024});
  process.stdout.write(r.stdout||'');process.stderr.write(r.stderr||'');
  const log=`${commandEvidence.length+1}-${label.replaceAll(/[^a-zA-Z0-9]+/g,'-')}.log`;
  writeFileSync(resolve(validationDir,log),(r.stdout||'')+(r.stderr||''));
  commandEvidence.push({label,command:cmd,args:argv,exitCode:r.status,error:r.error?.message,log});
  receipt.checks.push({label,exitCode:r.status,error:r.error?.message});save();
  if(r.error||r.status!==0)throw new Error(`${label} failed: ${r.error?.message||r.status}`);
  return r.stdout;
}
const sha=file=>createHash('sha256').update(readFileSync(file)).digest('hex');
save();
try {
  receipt.toolchain.cmake=query('cmake',['--version']);
  const library=platform.startsWith('windows')?'weapp_container_sdk.lib':'libweapp_container_sdk.a';
  for(const target of targets){
    const {receipt:manifest}=readNative(nativeRoot,target,[library]);
    readNotices(nativeRoot,licensesRoot,target);
    receipt.native.push(nativeIdentity(manifest));
  }
  let coreDir=resolve(nativeRoot,targets[0]);
  if(targets.length>1){coreDir=resolve(out,`.native-${platform}`);mkdirSync(coreDir,{recursive:true});
    run('lipo',['-create',...targets.map(t=>resolve(nativeRoot,t,library)),'-output',resolve(coreDir,library)],'Combine universal native archive');
    cpSync(resolve(nativeRoot,targets[0],'weapp_container_sdk.h'),resolve(coreDir,'weapp_container_sdk.h'));
  }
  const apple=platform.startsWith('macos')?['-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0',`-DCMAKE_OSX_ARCHITECTURES=${platform==='macos-universal'?'arm64;x86_64':platform==='macos-arm64'?'arm64':'x86_64'}`]:[];
  run('cmake',['-S',root,'-B',build,`-DWEAPP_CORE_DIR=${coreDir}`,`-DCMAKE_INSTALL_PREFIX=${prefix}`,'-DCMAKE_BUILD_TYPE=Release',...apple],'Configure desktop adapter');
  run('cmake',['--build',build,'--config','Release','--parallel','4'],'Build desktop adapter');
  run('cmake',['--install',build,'--config','Release'],'Install relocatable desktop SDK');
  const licenses=resolve(prefix,'share/weapp-container/licenses/native');mkdirSync(licenses,{recursive:true});
  for(const target of targets){const source=resolve(licensesRoot,target);if(!existsSync(resolve(source,'licenses.json')))throw new Error(`Native license inventory missing for ${target}; supply authorized runtime notices`);cpSync(source,resolve(licenses,target),{recursive:true});receipt.checks.push({label:`Native license inventory ${target}`,status:'included'});}
  if(existsSync(resolve(root,'LICENSE')))cpSync(resolve(root,'LICENSE'),resolve(prefix,'share/weapp-container/LICENSE'));
  cpSync(resolve(nativeRoot,'RUNTIME-LICENSE'),resolve(prefix,'share/weapp-container/RUNTIME-LICENSE'));
  for(const file of readdirSync(resolve(prefix,'lib/cmake/WeappContainerDesktop'))){const path=resolve(prefix,'lib/cmake/WeappContainerDesktop',file);if(readFileSync(path,'utf8').includes(root))throw new Error(`Installed CMake export leaks checkout path: ${file}`);}
  if(!args.includes('--skip-consumer')){
    run('cmake',['-S',resolve(prefix,'share/weapp-container/consumer'),'-B',consumer,`-DCMAKE_PREFIX_PATH=${prefix}`,'-DCMAKE_BUILD_TYPE=Release',...apple],'Configure installed independent consumer');
    run('cmake',['--build',consumer,'--config','Release','--parallel','4'],'Link installed C/C++ consumer without Rust');
    if(args.includes('--verify')){
      const binary=resolve(consumer,process.platform==='win32'?'Release/desktop_consumer.exe':'desktop_consumer');
      const explicitPack=option('--pack');
      const pack=resolve(out,`.pack-${platform}-${process.pid}`);
      if(existsSync(pack))throw new Error(`Consumer pack staging already exists: ${pack}`);
      copyPack(explicitPack,pack);
      receipt.packIntegrity=JSON.parse(readFileSync(resolve(pack,'manifest.json'),'utf8')).integrity.root;
      const report=resolve(prefix,'consumer-result.txt');
      const arch=option('--run-arch',process.arch);
      if(process.platform==='darwin'&&arch==='x86_64')run('arch',['-x86_64',binary,pack,report],'Execute installed consumer (x86_64; Rosetta on Apple Silicon)');
      else run(binary,[pack,report],`Execute installed native consumer (${process.arch})`);
      receipt.executedArchitecture=arch;receipt.displayBackend=process.env.GDK_BACKEND||null;
    }
  }
  for(const file of readdirSync(resolve(prefix,'lib'))){if(/\.(a|lib)$/.test(file))receipt.artifacts.push({file:`lib/${file}`,sha256:sha(resolve(prefix,'lib',file))});}
  receipt.status=args.includes('--verify')&&!args.includes('--skip-consumer')?'verified':'packaged';
} catch(error){receipt.status='failed';receipt.error=error.message;process.exitCode=1;}
receipt.finishedAt=new Date().toISOString();save();console.log(JSON.stringify({status:receipt.status,prefix,receipt:receiptPath,error:receipt.error},null,2));
