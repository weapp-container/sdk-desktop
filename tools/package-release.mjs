import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { assetEntry, cleanSource, readRelease, releaseUrl, run, sha256, versionOf, writeRelease, publishRelease } from './public-release.mjs';
const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2), option = name => { const i = args.indexOf(name); return i < 0 ? undefined : args[i + 1]; };
const out = resolve(option('--out') ?? join(root, 'artifacts/public')), version = versionOf(root);
const manifest = join(out, 'release.json');
if (args.includes('--publish') || args.includes('--verify')) {
  console.log(JSON.stringify(await publishRelease(root, manifest, args.includes('--publish')), null, 2));
} else if (option('--merge')) {
  if (existsSync(manifest)) throw Error('Use a fresh merged release directory');
  const assets = [];
  for (const path of option('--merge').split(',')) {
    const release = readRelease(resolve(path), 'weapp-container/sdk-desktop', version, true);
    if (release.sourceCommit !== cleanSource(root)) throw Error('Desktop targets have different source identities');
    for (const asset of release.assets) {
      if (existsSync(join(out, asset.fileName))) throw Error('Merge destination already contains an archive');
      const { copyFileSync } = await import('node:fs');
      mkdirSync(out, { recursive: true }); copyFileSync(join(dirname(resolve(path)), asset.fileName), join(out, asset.fileName));
      assets.push(asset);
    }
  }
  if (new Set(assets.map(asset => asset.platform)).size !== 3 || ['macos-universal', 'linux-x86_64', 'windows-x86_64'].some(platform => !assets.some(asset => asset.platform === platform))) throw Error('The three public Desktop targets are required');
  const release = writeRelease(root, out, 'weapp-container/sdk-desktop', assets);
  const mac = assets.find(asset => asset.platform === 'macos-universal');
  writeFileSync(join(out, 'WeappContainerDesktop.podspec'), `Pod::Spec.new do |s|\n  s.name = 'WeappContainerDesktop'\n  s.version = '${version}'\n  s.summary = 'Weapp Container native desktop SDK'\n  s.homepage = 'https://github.com/weapp-container/sdk-desktop'\n  s.author = 'Weapp Container contributors'\n  s.license = { :type => 'Apache-2.0', :file => 'share/weapp-container/LICENSE' }\n  s.source = { :http => '${releaseUrl(release, mac)}', :sha256 => '${mac.sha256}' }\n  s.platform = :osx, '12.0'\n  s.vendored_libraries = 'lib/libweapp_container_desktop.a', 'lib/libweapp_container_sdk.a'\n  s.preserve_paths = 'include/**/*', 'share/**/*'\n  s.libraries = 'c++', 'z', 'iconv', 'resolv'\n  s.frameworks = 'AppKit', 'Metal', 'QuartzCore', 'CoreGraphics', 'CoreText', 'CoreFoundation', 'Foundation', 'IOSurface', 'Security', 'SystemConfiguration', 'IOKit', 'AVFoundation', 'CoreMedia', 'CoreVideo', 'CoreLocation', 'CoreWLAN', 'CoreImage', 'CoreBluetooth', 'Contacts', 'EventKit', 'LocalAuthentication'\n  s.pod_target_xcconfig = { 'HEADER_SEARCH_PATHS' => '\"$(PODS_TARGET_SRCROOT)/include\"', 'CLANG_CXX_LANGUAGE_STANDARD' => 'c++17' }\nend\n`);
} else {
  cleanSource(root);
  if (!option('--sdk')) throw Error('--sdk must select a verified installed Desktop SDK');
  const input = resolve(option('--sdk'));
  const receipt = JSON.parse(readFileSync(join(input, 'package.json')));
  if (receipt.sdkVersion !== version || receipt.status !== 'verified' || receipt.sourceDirty !== false || receipt.sourceCommit !== cleanSource(root)) throw Error('A clean, consumer-verified Desktop package is required');
  const platform = ({'linux-x64':'linux-x86_64','windows-x64':'windows-x86_64'})[receipt.platform] ?? receipt.platform;
  if (!['macos-universal', 'linux-x86_64', 'windows-x86_64'].includes(platform)) throw Error('Unexpected public desktop platform');
  for (const entry of receipt.artifacts) if (sha256(readFileSync(join(input, entry.file))) !== entry.sha256) throw Error(`Desktop file changed: ${entry.file}`);
  mkdirSync(out, { recursive: true });
  const name = `WeappContainerDesktop-${version}-${platform}.tar.gz`;
  if (existsSync(join(out, name)) || existsSync(manifest)) throw Error('Use a fresh release output directory');
  run('tar', ['-czf', join(out, name), '-C', input, '.'], root);
  writeRelease(root, out, 'weapp-container/sdk-desktop', [assetEntry(out, name, { platform, component: 'desktop-sdk' })]);
}
