<a id="maintaining-bilingual-documentation"></a>
# 双语文档维护

**简体中文** · [English](README.en.md)


`documentation.json` 是本仓库自有文档的配对与分类清单。默认正文为 `*.md`（中文），对应英文为 `*.en.md`；API 名称、命令、包名和代码示例保留原名。第三方原文、许可文本、历史证据和固定测试输入用精确 SHA-256 排除，不能因翻译而修改。

<a id="update-a-document-pair"></a>
## 更新一组文档

1. 修改中文及英文，并保留顶部语言链接。
2. 为被改写的旧标题保留 `<a id="old-heading"></a>`。两种语言的对应章节应能接收相同的链接片段。
3. 新文档在清单中登记稳定 `id`、分类、两种语言路径及来源身份。不要把新增自有文档排除为历史输入。
4. 按当前中文逐项复核英文中的版本、参数、许可、平台状态、限制和代码，再记录摘要及源哈希。

```sh
node tools/documentation/review.mjs --page docs/example.md --chinese-summary "本页已复核的中文内容摘要"
node tools/documentation/check.mjs
```

摘要是复核记录，不是机器翻译质量的保证。不要只刷新哈希来消除 CI 错误。核心 checkout 还可运行 `node tools/documentation/check-workspace.mjs` 检查固定 SDK 和网站；组织 `.github` 在自己的 CI 独立检查。

<a id="website-snapshots"></a>
## 网站快照

工程文档正文只在来源仓库维护。先提交来源文档，再在 website 执行 `tools/documentation-snapshot.mjs`，导入固定提交的清单、原文和必要图片。导入器只接受清单登记的文档，源码链接固定到相同提交。

网站独立构建核验原始快照哈希、派生页面哈希、资源和完整文件集合，不需要父仓库、GitHub token 或私有归档。原始 API 快照独立保存，翻译仅在展示层映射；不要修改它的支持等级或哈希来配合翻译。

<a id="checks-and-tests"></a>
## 检查与测试

```sh
node --test tools/documentation/check.test.mjs
node tools/documentation/check.mjs
```

检查器拒绝缺少配对、过期中文身份、未分类文档、错误语言链接、失效相对链接、损坏章节和被修改的固定输入。网站另行验证完整路由、搜索、SEO、代码高亮与快照。需要平台工具链的示例按实际环境记录验证范围，不把文档修改描述为新增平台支持。
