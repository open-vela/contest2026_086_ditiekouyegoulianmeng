---
name: openvela-build-traps
description: "openvela/NuttX 构建与配置的隐蔽陷阱：增量编译不跟踪头文件依赖导致陈旧 .o、clean 不是全量清理、defconfig 与 .config 不同步、build.sh 会覆盖板级 defconfig、distclean 后第三方 app 补丁打不上。Use when: 改了头文件后行为诡异、结构体字段读出垃圾值、编译过了但板子上没变化、提交的 defconfig 和实际构建不一致、清理不干净、distclean 之后编译失败。"
---

# openvela / NuttX 构建陷阱

官方 `openvela-build` 技能覆盖了正常编译流程。这份补的是**编译「成功」但结果是错的**
那一类问题——没有报错，没有警告，只是行为不对。

## 1. 改了头文件，行为不对：陈旧 `.o`

**症状**：往结构体里加了个字段，编译通过，运行起来读那个字段是垃圾值（我们遇到的是
一个长度字段读出 240），或者结构体大小对不上导致越界。

**原因**：这套 Makefile **不跟踪 `.h` 依赖**。改了 `foo.h`，依赖它的 `bar.c` 不会重编，
链接进镜像的还是按旧结构体布局编译出来的 `.o`。

**修法**：

```bash
make -C nuttx distclean
```

改任何**被多个模块共享的头文件**之后都要做。只改 `.c` 不用。

## 2. `make clean` 不是全量清理

`clean` 会留下 `registry/` 和 `builtin_list.c`。这两个文件决定「哪些 app 被注册进
系统」，所以你**新增或删除一个 app 之后**，光 `clean` 是不够的，镜像里 app 列表还是旧的。

要全量就是 `distclean`：

```bash
make -C nuttx distclean
```

## 3. `defconfig` 与 `.config` 是两份东西，会不同步

- `boards/<...>/configs/<config>/defconfig` —— 进仓库的，别人靠它复现你的构建
- `nuttx/.config` —— 实际在用的

**用 menuconfig 改过配置之后，`.config` 变了但 `defconfig` 没变。** 如果你把 menuconfig
的改动直接当作成果提交，别人拉下来编出来的东西和你的不一样。

正确做法是让 `savedefconfig` 重新生成：

```bash
make -C nuttx savedefconfig   # 从 .config 生成 nuttx/defconfig
```

然后把它拷回板级目录，**并且 `git diff` 看一遍**——`savedefconfig` 的输出会丢掉
所有 `# CONFIG_X is not set` 里默认值的部分，也可能顺手删掉一些你以为还在的项。

## 4. `build.sh` 会覆盖你传进去的那个 defconfig

这条最容易踩，因为完全在背后发生。`build.sh` 最后一步是：

```bash
if [ ! -d $1 ]; then
  cp ${NUTTXDIR}/defconfig ${ROOTDIR}/nuttx/boards/*/*/${1/[:|\/]//configs/}
else
  cp ${NUTTXDIR}/defconfig $1          # ← 传目录进去时走这条
fi
```

也就是说：**`./build.sh <某个目录>` 结束时，会把 `savedefconfig` 的结果写回
`<某个目录>/defconfig`，覆盖掉你仓库里的那份。**

后果分两种：

- 如果那个 defconfig 正是你在维护的板级配置 → 这是好事，它会自动保持同步，但你会
  在 `git status` 里看到意料之外的改动。
- 如果你的工作区正好 checkout 在一个只改了 18 行、准备提 PR 的分支上 → 这份干净的
  小改动会被整个 `.config` 的 `savedefconfig` 输出冲掉。

**提分支之前先 `git status`，编完再看一次。**

## 5. `build.sh` 能吃任意含 defconfig 的目录

`configure.sh` 有一个「custom configuration」回退分支：当
`boards/*/*/<boarddir>/configs/<configdir>` 不存在时，它会直接拿你给的路径当
`boardconfig`。所以板级配置**不一定非得放在 `nuttx/boards` 下**——放在任何仓里都能编：

```bash
./build.sh vendor/<厂商>/boards/<板>/configs/<配置>
```

这对「板级适配也想收进自己仓库」的场合有用。

## 6. `distclean` 会删掉下载型第三方 app，重建时可能失败

有些 app 不是源码进仓，而是在构建时下载 tarball 再打补丁。`distclean` 会把它们和
tarball 一起删掉：

```make
distclean::
	$(call DELDIR, $(QUICKJS_UNPACK))
	$(call DELFILE, $(QUICKJS_TARBALL))
```

于是下一次构建会重新下载。**如果上游把那版 tarball 换了（或者补丁本来就是针对一个
被改过的源码树生成的），`patch` 就会失败**：

```
patching file quickjs-libc.c
Hunk #1 succeeded at 3558 (offset -137 lines).
Hunk #3 FAILED at 3743.
1 out of 3 hunks FAILED -- saving rejects to file quickjs-libc.c.rej
make[3]: *** [Makefile:99: quickjs] Error 1
```

注意这在 `make -C apps context` 阶段就炸了，报错长得像是你的 app 有问题，其实跟
你的代码没关系。

**处理**：看 `.rej` 里被拒绝的那段想干什么，手工应用等效改动，然后

```bash
touch <app>/<unpack_dir>/.patch     # 让 make 认为已就绪
```

再重跑构建。**不要**去改那个补丁文件然后又 `distclean`——下次下载可能又对得上，
你就白改了。

## 排查顺序

构建成功但结果不对时，按这个顺序怀疑：

1. 改过头文件吗？→ `distclean`
2. 增删过 app 吗？→ `distclean`
3. 只用 menuconfig 改过配置吗？→ `savedefconfig` 并 diff
4. 刚 `distclean` 过吗？→ 检查第三方 app 的补丁有没有打上
5. 以上都不是 → 才去怀疑代码本身
