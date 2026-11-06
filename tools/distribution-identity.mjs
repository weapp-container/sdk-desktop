/** Explicit distribution identity, independent of release/build profiles. */
export function distributionIdentity(value = {}) {
  const distributionProfile = value.distributionProfile ?? 'full';
  const policySha256 = value.policySha256 ?? null;
  if (!['full', 'restricted-showcase'].includes(distributionProfile)
    || (distributionProfile === 'full' ? policySha256 !== null : !/^[a-f0-9]{64}$/.test(policySha256 ?? ''))) {
    throw new Error('Invalid distribution profile or showcase policy SHA-256');
  }
  return { distributionProfile, policySha256 };
}
export function assertDistributionIdentity(actual, expected = {}) {
  const left = distributionIdentity(actual), right = distributionIdentity(expected);
  if (left.distributionProfile !== right.distributionProfile || left.policySha256 !== right.policySha256) {
    throw new Error('Distribution profile or showcase policy SHA-256 mismatch');
  }
  return left;
}
export function distributionOptions(args) {
  const read = name => {
    const indexes = args.flatMap((value, index) => value === name ? [index] : []);
    if (indexes.length > 1) throw new Error(`Duplicate ${name}`);
    if (!indexes.length) return undefined;
    const value = args[indexes[0] + 1];
    if (!value || value.startsWith('--')) throw new Error(`${name} requires a value`);
    return value;
  };
  return distributionIdentity({ distributionProfile: read('--distribution-profile'), policySha256: read('--showcase-policy-sha256') });
}
// Preserve historical full installer identities; restricted installs always carry both fields.
export function installationDistribution(value = {}) {
  const identity = distributionIdentity(value);
  return identity.distributionProfile === 'full' ? {} : identity;
}
export function verifyDistributionArtifact(bytes, value) {
  const identity = distributionIdentity(value);
  const marker = Buffer.from(`weapp-container-distribution:${identity.distributionProfile}:${identity.policySha256 ?? ''}\0`);
  if (!Buffer.from(bytes).includes(marker)) throw new Error('Compiled runtime is missing the exact distribution policy identity');
  return identity;
}
