<a id="双语文档维护"></a>
# Maintaining bilingual documentation

[简体中文](README.md) · **English**


`documentation.json` classifies and pairs this repository's authored documents. Default `*.md` files are Chinese; their English counterparts use `*.en.md`. Keep API names, commands, package names, and executable examples unchanged. Third-party texts, license originals, historical evidence, and fixed test inputs have exact SHA-256 exclusions and must not change during translation.

<a id="更新一组文档"></a>
## Update a document pair

1. Update both languages and retain the language links at the top.
2. Preserve renamed heading fragments with `<a id="old-heading"></a>`. Corresponding sections should accept the same fragments in both languages.
3. Register new documents with stable IDs, categories, language paths, and source identities. Do not exclude new authored documents as historical inputs.
4. Review English versions, parameters, licensing, platform status, limitations, and code against the current Chinese page, then record its summary and source hash.

```sh
node tools/documentation/review.mjs --page docs/example.md --chinese-summary "本页已复核的中文内容摘要"
node tools/documentation/check.mjs
```

The summary records a review; it does not guarantee machine translation quality. Do not refresh hashes merely to silence CI. The core checkout can also run `node tools/documentation/check-workspace.mjs` to check its pinned SDKs and website. The organization's `.github` repository runs its own checks.

<a id="网站快照"></a>
## Website snapshots

Maintain engineering prose only in its source repository. Commit source changes before running `tools/documentation-snapshot.mjs` in website to import pinned catalogs, originals, and required images. The importer admits only cataloged documents and pins source links to the same commit.

An independent website build verifies original and derived hashes, resources, and the complete file set without its parent repository, a GitHub token, or private archives. API snapshots remain independent; translations are presentation mappings. Do not change support levels or snapshot hashes to accommodate translations.

<a id="检查与测试"></a>
## Checks and tests

```sh
node --test tools/documentation/check.test.mjs
node tools/documentation/check.mjs
```

The checker rejects missing pairs, stale Chinese identities, unclassified documents, wrong-language links, broken relative links or fragments, and modified fixed inputs. Website checks cover complete routing, search, SEO, code highlighting, and snapshots. Record the actual scope of examples that need platform toolchains. Documentation changes do not establish new platform support.
