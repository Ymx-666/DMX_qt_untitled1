# DMX 每次改动标准流程

版本：1.0

生效日期：2026-09-06

适用项目：`/home/sht/work/DMX_qt`

远程仓库：`Ymx-666/DMX_qt_untitled1`

## 1. 这套流程解决什么问题

DMX 同时涉及实时图像、UDP、共享盘、转台、YOLO、全景缓存、雷达 UI 和现场数据。一次看似很小的修改也可能影响正式采集，因此每次改动都要能回答五个问题：

1. 为什么改，验收条件是什么？
2. 改了哪些文件和行为？
3. 用哪个提交、配置、模型和数据测试？
4. 测试是否通过，证据在哪里？
5. 出现问题时如何回到上一个可用版本？

标准路线为：

```text
Issue -> 开发分支 -> 实现与针对性自测 -> 候选 Commit
      -> 完整验证/Run ID -> 报告 Commit -> Push -> PR
      -> CI/审核 -> 合并 -> 同步 main -> 构建/部署 -> Release
```

纯粹询问代码、查看日志或只读诊断不产生改动时，可以停在调查阶段。只要要修改文件、系统配置、部署状态或数据，就必须从 Issue 开始。

## 2. 单人项目如何分工

目前只有一名项目负责人，不代表可以跳过复核，而是把职责分开执行：

| 角色 | 负责人 | 必须完成的工作 |
|---|---|---|
| 产品与验收 | 用户 | 明确目标、确认关键取舍、批准 PR、安排真机测试和正式发布 |
| 开发与维护 | Codex | Issue、分支、实现、测试、报告、Commit、PR、CI 跟踪和合并后清理 |
| 独立复核 | 自动测试 + 固定回放 + 截图 + 用户审核 | 防止只凭“本机看起来正常”直接发布 |

用户通常只需要做三件事：说明需要达到的效果；在必须人工判断时查看结果；在 GitHub 批准并合并 PR。其余可重复的工程工作由 Codex执行并汇报。

## 3. 第一步：检查现场

每项任务开始时先记录当前事实：

```bash
cd /home/sht/work/DMX_qt
git status --short --branch
git log -1 --oneline --decorate
pgrep -af '(^|/)(DMX|DMX_test)( |$)' || true
mountpoint /mnt/dmx_share
mountpoint /mnt/dmx4t
```

检查原则：

- 已有未提交文件默认属于用户，不能擅自恢复、覆盖或顺手提交。
- 无关修改保留在工作区，并在提交时明确排除。
- 如果已有 DMX 正在采集，不停止、不重启，也不抢占端口。
- 只有任务依赖共享盘或数据盘时，挂载失败才阻塞相关测试。
- 禁止把密码、令牌或设备凭据写进任何项目文件和 GitHub 内容。

## 4. 第二步：建立 Issue

Issue 是改动的身份证。标题使用类型和编号：

```text
[BUG-xxx] 修复明确错误
[UI-xxx] 调整界面和交互
[ALG-xxx] 修改检测、阈值、融合或模型
[PERF-xxx] 优化延迟、队列、CPU/GPU或内存
[OPS-xxx] 修改挂载、启动、部署、端口或日志
[DATA-xxx] 采集、清洗、标注或数据集变更
[DOC-xxx] 文档修改
[REFACTOR-xxx] 不改变行为的代码整理
```

每个 Issue 至少写明：

```markdown
## 背景
问题或需求发生在哪里。

## 范围
本次修改什么，明确不修改什么。

## 验收标准
- 可以客观判断通过或失败的条件。

## 风险
是否影响实时性、识别率、设备、数据、兼容性或正式采集。

## 测试计划
需要执行哪些单测、构建、回放、截图或真机测试。
```

一个 Issue 只解决一个主要问题。需求过大时拆成多个 Issue，避免一次 PR 同时修改算法、UI、部署和文档。

## 5. 第三步：创建开发分支

先从最新 `main` 建立短期分支：

```bash
git switch main
git pull --ff-only
git switch -c ui/UI-123-target-panel
```

分支命名：

```text
feature/<Issue>-<short-name>
fix/<Issue>-<short-name>
ui/<Issue>-<short-name>
alg/<Issue>-<short-name>
perf/<Issue>-<short-name>
ops/<Issue>-<short-name>
docs/<Issue>-<short-name>
```

约束：

- 不直接在 `main` 上编辑和提交。
- 不使用强制推送覆盖远端历史。
- 一个分支对应一个 Issue，通常在 1 至 3 天内完成。
- 切换分支前后都检查工作区，确保用户未提交修改仍然存在。

## 6. 第四步：实现改动

### 6.1 通用要求

- 先读现有代码和测试，沿用当前模块边界和实现模式。
- 修改范围围绕验收条件，不夹带无关重构。
- 增加或调整行为时同步补测试、日志和必要文档。
- 参数写清单位、默认值和有效范围；协议、端口、路径变化必须显式记录。
- 发现任务外问题时新建 Issue，不在当前分支顺手扩大范围。

### 6.2 DMX 与 DMX_test 的关系

两个程序共用大部分 C++ 源码，但运行环境不同：

- `DMX` 是现场设备版，读取 `/mnt/dmx_share`，可能控制真实设备。
- `DMX_test` 是固定数据回放版，用 2026-07-23 数据模拟 8 秒/圈。

算法和 UI 通常先在 `DMX_test` 验证，再同步正式行为。修改共用源码或条件编译时，必须同时构建两个程序，检查是否出现只在某个模式下生效、编译或显示异常的问题。

### 6.3 不同类型改动的重点

| 类型 | 必须关注 |
|---|---|
| UI | 常用分辨率、文本溢出、控件遮挡、目标联动、ROI 缩放、正式/回放两种构建 |
| 算法 | 固定数据前后指标、误报/漏报、RGB/BW 融合、事件去重、推理时延和队列 |
| Mask/全景 | 前三圈门控、后台线程、360 度左右环绕、缓存长度和丢帧 |
| 设备/协议 | 端口方向、指令应答、超时、停止保护、断网和共享盘异常 |
| 数据 | 原始数据只读、来源、日期、manifest、校验值和可重建性 |
| 运维 | 启动前检查、锁、日志、磁盘、回滚和不打断当前运行 |

## 7. 第五步：检查并提交候选 Commit

先执行与本次改动直接相关的针对性自测，再检查本任务的 diff：

```bash
git status --short
git diff --check
git diff -- path/to/file
```

只暂存当前任务文件：

```bash
git add path/to/file1 path/to/file2
git diff --cached --check
git diff --cached --stat
git commit -m "fix(pano): wrap target crop across panorama boundary"
```

常用提交类型：

```text
feat  fix  perf  refactor  test  docs  ops  build
```

禁止事项：

- 禁止为省事执行 `git add -A` 后不检查内容。
- 禁止提交构建目录、运行日志、原始图像、模型、虚拟环境和密码。
- 禁止回退或提交任务开始前已有的无关修改。
- 禁止用一个模糊提交同时装入多个不相关任务。

这个提交是后续完整验证绑定的候选版本。完整验证失败时继续在同一分支修复并创建新提交，再使用新的 Run ID 重新验证。

## 8. 第六步：执行完整验证并生成 Run ID

Run ID 把一次测试与代码和证据绑定，推荐格式：

```text
<Issue>_<purpose>_<YYYYMMDD_HHMMSS>
```

例如：

```text
UI123_validation_20260906_103500
ALG021_baseline_20260907_091000
ALG021_candidate_20260907_103000
```

统一测试入口：

```bash
./scripts/test_fast.sh
./scripts/test_build.sh
./scripts/test_replay_smoke.sh
```

测试选择矩阵：

| 改动范围 | 最低要求 |
|---|---|
| 仅文档 | `git diff --check`，人工核对命令、链接、日期和当前事实 |
| Python 工具 | `test_fast.sh`，相关专项测试 |
| C++/配置/脚本 | `test_fast.sh` + `test_build.sh` |
| UI | 快速测试 + 双构建 + QtTest + 正式界面截图检查 |
| 算法/融合/队列/Mask/全景 | 快速测试 + 双构建 + 固定回放 |
| 性能 | 上述测试 + 同输入前后 p50/p95、最大队列、丢帧、CPU/GPU 对比 |
| 设备/挂载/转台 | 离线测试通过后，由用户安排真机窗口并记录结果 |
| 正式发布 | 全部适用测试 + 启动冒烟 + 回滚准备 |

固定回放默认使用：

- UDP 原始日志：`/mnt/dmx4t/data/raw_log/20260723`
- 回放图像：`/mnt/dmx4t/data/replay_sources/baseline_20260723`
- 速度：8 秒/圈

测试结果默认保存在：

```text
test-results/<Run ID>/
```

大型证据可放到：

```text
/mnt/dmx4t/data/dmx_test/validation/<Run ID>/
```

同一个 Run ID 不得重复使用或覆盖。测试失败不能删除证据，应修复后创建新的 Run ID。

## 9. 第七步：编写并提交验证报告

报告放在 `docs/testing/`，推荐命名：

```text
<Issue>_<日期>验证报告.md
```

最低内容：

```markdown
# 验证报告

- Issue：
- 分支/Commit：
- Run ID：
- 测试时间和主机：
- DMX/DMX_test 模式：
- 配置 SHA-256：
- 模型路径、模型 ID 和 SHA-256：
- 数据集版本：

## 改动摘要
## 执行命令
## 测试结果
## 指标前后对比
## 日志和截图位置
## 未执行项与残余风险
## 回滚方法
```

不能用“测试正常”代替数值和证据。真机未测、共享盘不可用或只做了离屏 UI 测试时必须明确写出。

报告记录的 Commit 必须是刚完成完整验证的候选 Commit。报告完成后单独提交：

```bash
git add docs/testing/<report>.md
git commit -m "docs(test): record <Issue> validation results"
```

## 10. 第八步：推送并创建 PR

```bash
git push -u origin <branch>
```

PR 必须包含：

- `Closes #<Issue编号>`。
- 修改原因和用户可见行为。
- 关键实现说明。
- 风险、兼容性和回滚方法。
- 测试命令、结果和 Run ID。
- UI 前后截图或算法前后指标。
- 配置、模型、数据、端口和路径是否变化。

提交 PR 后等待：

- `Fast tests` 通过。
- `Linux builds` 通过。
- PR 状态可合并且没有冲突。
- 用户审核并明确同意合并。

Codex 默认不能替用户点击合并。CI 失败时继续在原分支修复、重新测试和推送，不另开绕过测试的 PR。

## 11. 第九步：合并后的同步和清理

用户确认 PR 已合并后执行：

```bash
git fetch --prune origin
git switch main
git merge --ff-only origin/main
git branch -d <branch>
git push origin --delete <branch>
```

然后再次确认：

```bash
git log -1 --oneline --decorate
git status --short --branch
```

工作区可以保留用户未提交内容，但必须逐项说明；不能错误地报告为“工作区干净”。

## 12. 第十步：构建、部署与 Release

### 12.1 普通合并后的构建

正式 DMX 改动：

```bash
./build_linux.sh
```

回放程序改动：

```bash
./build_test.sh
```

构建时如果旧 DMX 进程仍在运行，不直接终止。Linux 允许新二进制替换磁盘文件，但旧进程会继续执行旧代码；用户结束并从桌面重新启动后新版本才生效。

### 12.2 正式发布

不是每个 PR 都必须增加产品版本号，但每个合并必须有明确部署状态：`未部署`、`测试环境` 或 `正式环境`。用户决定对外发布时再执行：

1. 选择已合并且全部验证通过的 `main` 提交。
2. 记录程序、配置、模型和数据集版本及 SHA-256。
3. 完成固定回放、必要的真机验收和启动冒烟。
4. 创建版本标签，例如 `v0.9.1`。
5. 保存发布说明、二进制校验值、报告位置和回滚版本。
6. 部署后观察日志、队列、丢帧、推理延迟和设备状态。

### 12.3 回滚

- 源码回滚使用新的 `git revert` 提交和 PR，不重写 `main` 历史。
- 现场回滚应恢复上一套已验证的程序、配置和模型组合。
- 先保留故障日志、Run ID 和运行 manifest，再切回旧版本。
- 回滚后执行启动冒烟，并新建 BUG Issue 分析根因。

## 13. 紧急故障流程

设备失控、数据损坏风险、磁盘写满或程序完全不可用时，安全处置优先：

```text
停止危险操作 -> 保留日志和现场 -> 恢复上一可用版本
-> 创建 P0/P1 Issue -> hotfix 分支 -> 最小修复
-> 测试 -> PR -> 审核 -> 发布 -> 复盘并补自动测试
```

紧急情况允许先停止设备或回滚，但不能永久跳过 Issue、测试记录和复盘。

## 14. 每次任务完成检查表

```text
[ ] Issue 有背景、范围、验收标准、风险和测试计划
[ ] 从最新 main 创建任务分支
[ ] 用户已有无关修改未被覆盖或提交
[ ] 实现范围与 Issue 一致
[ ] DMX 与 DMX_test 的影响已同时检查
[ ] 必要的快速测试、双构建、回放、截图或真机测试已完成
[ ] 测试有唯一 Run ID，报告能定位日志和证据
[ ] Commit 聚焦且不包含数据、模型、密码和构建产物
[ ] PR 关联 Issue，CI 全部通过
[ ] 用户已审核并决定是否合并
[ ] 合并后本地 main 已同步，任务分支已清理
[ ] 需要的程序已构建，部署状态和重启要求已说明
[ ] 正式发布时已有版本、校验值和回滚点
```

## 15. 新对话如何自动使用这套流程

项目根目录的 `AGENTS.md` 是 Codex 的持久规则。以后应从以下目录开启 DMX 对话：

```bash
cd /home/sht/work/DMX_qt
```

只要新对话的工作目录位于该项目中，Codex 就会自动发现 `AGENTS.md`，先读取当前交接和本流程，再开始工作。若对话从其他目录启动，应先切换到本项目目录；这套规则只针对 DMX，不作为其他项目的全局规则。

相关文档：

- `AGENTS.md`：新对话自动读取的强制规则。
- `TASK_HANDOFF.md`：当前代码、依赖和运行基准。
- `docs/项目管理/DMX项目工程化管理策略.md`：完整工程治理设计。
- `docs/testing/README.md`：测试入口与结果目录。
