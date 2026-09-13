---
name: openvela-contest-submit
description: "向 open-vela 生产仓提 PR 前必须知道的闸门：repository ruleset 与 branch protection API 的区别、cla/signature 对所有分支生效、只能 rebase 合并导致提交 SHA 被重写、CODEOWNERS 挡住自己合并、checkpatch 对非 nuttx 仓只查 Change-Id、提交邮箱必须与 CLA 一致。Use when: push 被拒 GH013、提示 Required status check cla/signature is expected、Changes must be made through a pull request、判断 PR 到底合没合、PR 一直没人合、CI 卡在 setup。"
---

# openvela 生产仓的提交闸门

官方 `submit-pr` 技能讲了 fork + PR 的**操作**。这份讲的是**为什么你的 PR 推不上去 /
合不了 / 看起来合了其实没合**。

## 1. 规则在 ruleset 里，不在 branch protection API 里

这是最容易误判的一条。查分支保护：

```
GET /repos/{owner}/{repo}/branches/{branch}/protection
```

返回「没有保护」——**但分支照样推不上去**。因为限制来自 **repository ruleset**，
那是个独立的接口：

```
GET /repos/{owner}/{repo}/rules/branches/{branch}      # 匿名可读
GET /repos/{owner}/{repo}/rulesets/{id}
```

**永远用 ruleset 接口判断能不能推。** 我们就是先看了 branch protection 以为没事，
推的时候吃了 GH013。

## 2. `cla/signature` 可能对所有分支生效

典型 ruleset 内容：

```json
{
  "required_status_checks": [{ "context": "cla/signature" }],
  "required_approving_review_count": 0,
  "require_code_owner_review": false,
  "allowed_merge_methods": ["rebase"],
  "require_extra_approval_for_unattributed_changes": true
}
```

注意它**没有 `conditions` 限定分支**——也就是对**所有分支**生效。后果是：

- 直推 `main` 被拒
- **推自己的 `feat/*` 分支到 origin 也被拒**（"Required status check cla/signature is expected"）

因为状态检查只对 PR 跑，而你推的是普通分支，永远等不到那个检查。

**做法：推到自己 fork 的 remote，从 fork 开 PR。**

## 3. 只能 rebase 合并 → 提交 SHA 会被重写

`allowed_merge_methods: ["rebase"]` 意味着合并时每个提交都被**重新应用**，SHA 全新。

后果：**`git merge-base --is-ancestor <你的分支头> origin/main` 会返回「不是祖先」，
但 PR 其实早就合了。**

判断合没合，只看 PR 本身的 `merged_at`：

```
GET /repos/{owner}/{repo}/pulls/{n}
```

**推论**：主线每合一次 PR，你手头的特性分支就作废了，必须
`git rebase origin/<branch>` 再强推。别指望 `merge-base` 对得上。

## 4. CODEOWNERS 挡住自己合并

生产仓的 `CODEOWNERS` 通常长这样：

```
* @open-vela/dev-ai-contest-reviewer @open-vela/openvela-reviewer
```

`require_code_owner_review: true` 时**外部贡献者永远合不了自己的 PR**。这不是 bug，
是设计。别在 CI 绿了之后一直刷页面等它自己合。

**含义**：如果你手上还有别的交付方式（比如一个能直接烧的固件），**以那个为准**，
不要把「PR 合入」当成交付的前置条件。

## 5. `checkpatch` 对非 nuttx 仓只查一件事

`open-vela` 各仓的 CI 里，`checkpatch` 对**非 nuttx 仓**只检查提交信息里有没有
**Gerrit 的 `Change-Id:` 行**（那是 Gitee/内部流程的产物）。有就拒，没有就过。

也就是说：**commit message 格式、行长、签名都不查**。别为了过 checkpatch 反复重写
提交信息——先确认你的仓到底是哪一种。

## 6. 提交邮箱必须和 CLA 一致

`require_extra_approval_for_unattributed_changes: true` 的 ruleset 下，
**提交者邮箱和 CLA 登记的邮箱不一致**会额外卡一道。

```bash
git log -1 --format='%an <%ae>'
```

不对就：

```bash
git commit --amend --reset-author      # 改最近一次并重置作者
```

改完要**强推**，并且**所有提交**都要对——不只是最后一个。

## 7. CI 会全量拉工程再编

`ci.yml` 通常不是只编你改的仓，而是：

```
repo init -u https://github.com/open-vela/manifests -b "$PR_BASE_REF" -m openvela.xml --group=default,platform-linux --depth=1 --git-lfs
repo sync
<全量编译>
```

所以 CI 慢是正常的（几十分钟起）。它还会解析 PR 正文里的：

```
depends-on: [owner/repo/pull/N]
```

把依赖的 PR 分支一起拉进构建。**只有确认依赖真的能被拉到、且分支名对得上时才写**——
写错了 CI 会以更难懂的方式失败。不确定就别写。

## 8. 参赛场景下的一条重要事实

大赛规则是：**fork 专属仓 → PR 回专属仓 → 自行合入**；改动公共仓（nuttx /
`packages_*` / `vendor_*`）走 fork + PR 到 `dev-ai-contest-2026`，**由组委会 review 合入**。

**上游 PR 只在获奖后才强制要求。** 所以：

- 交赛截止前，「作品进专属仓 + 可运行产物 + 文档」才是硬指标
- 上游 PR 是加分项和获奖后的义务，不是交赛的前置条件
- **别把时间全花在等上游 review 上**

## 自查清单

提 PR 前逐条过：

- [ ] 分支推在 **fork** 上，不是 origin
- [ ] 提交邮箱 = CLA 邮箱，**所有提交**都对
- [ ] 没有一个提交信息里有 `Change-Id:`
- [ ] 基于**目标分支最新头** rebase 过（不是基于几天前的快照）
- [ ] `depends-on` 只在确定时才写
- [ ] 目标仓的 `CODEOWNERS` 会不会挡住自己合入 → 会的话，另留交付路径
