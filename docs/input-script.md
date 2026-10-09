# 输入脚本与磁带（注入按键 + 断言）

用于在没有人工操作的情况下驱动游戏并验证结果，适合做「改数值 → 立即回归」和后续的自动化测试，
也是「手工玩一段 → 沉淀成回归用例」的唯一入口。

- 由宏 `MONO_DEBUG_INPUT` 控制编译；关闭时调用点被宏展开为空，运行期零成本
- 脚本由 `input_script <路径.txt>` 显式指定；不传就是正常手动游玩
- 指定的脚本会在启动后执行，**跑完自动退出**：退出码 `0` = 全部断言通过，`1` = 有断言失败或脚本没载入
  （脚本载入失败会立即以退出码 1 收摊，不会静默掉进手动游玩 —— 那对自动化是最难发现的一种失败）
- 回归怎么跑：`test\run_tests.bat <用例名|all> [fast]`（**串行**，一次一条）——
  不带 `fast` 是实时跑（人看着渲染）；带 `fast` 则加 `--fast` **且每条用例跑两遍、逐字节对拍两遍的 `--trace`**
  （确定性守卫：拓「没进快照却仍影响逻辑」的状态）；AI/自动回归用 `all fast`
- `--fast` 让逻辑步不吃真实时间（每循环正好一步）且改成每 64 帧才 Present 一次：单条 1961 帧的用例从 33 秒降到 1.0 秒，
  实测与实时模式的逐帧轨迹**逐字节一致**（只改节奏、不改逻辑）；但它只能配磁带用，手动游玩会被快进到不可玩
  - 它**不是无头模式**：窗口照开、帧绘制列表照常构建与提交（只有渲染时才暴露的问题不会因此被漏掉）
  - 快慢全在 `Present` 上：实测同一用例「每帧呈现 14.03s / 每 16 帧 2.02s / 每 64 帧 1.01s / 从不呈现 1.01s」
    —— 我们自己的绘制提交不是瓶颈，DXGI 翻转队列的限速才是
- 热键 `F7` 打印当前逻辑步帧号与完整角色状态，写新脚本时用它取样

## 1. 三个概念

| 概念 | 住在哪 | 回答什么问题 |
| --- | --- | --- |
| **存档** | `include/save.h`（正式系统） | 世界从哪来（详见 `docs/save-system.md`） |
| **磁带** | `include/debug/replay.h` + `src/debug/replay.cc` | 从那一刻起，输入是什么、要在哪些帧断言什么 |
| **文本脚本** | 本文件 / `src/debug/input_script.cc` | 磁带的文本形式（人写、人读、进 git） |

磁带是唯一的**内存表示**：存档点 + **输入变化点** + 断言操作。三种用法只是同一盘磁带的
三种来源与两种终点 —— 所以「录一段 → 导出脚本 → 当回归跑」之间不会有语义漂移：

| 用法 | 磁带从哪来 | 存档点 | 终点 |
| --- | --- | --- | --- |
| 录制（`F5`） | 真实设备每逻辑步记录（连续相同的帧折叠成一个变化点） | 按下 F5 时写 `build/replay_state.sav` | 再按 `F5` 结束 → 自动导出 `build/replay_script.txt` |
| 回放（`F6`） | 本次会话录的那盘；没有就载入最近导出的脚本 | 磁带自带 | 循环播放（每圈打一行 `TAPE: cycle N PASS/FAIL`），再按 `F6` 停止 |
| 脚本（`input_script`） | 文本载入 | 文本第一行的 `save` | 跑完退出并给出退出码 |

磁带只记**变化点**而不是逐帧输入：连续相同的帧折叠掉，所以内存与文本是同一量级
（基线回归 1961 帧只有 104 个变化点）。摇杆、扳机与鼠标也在磁带里（所以回放能带它们），
但文本语法只能表达动作 —— 录制里出现模拟量时导出会 `WARN` 说明它被丢掉了。

## 2. 输入源（互斥）

同一时刻只有一个源在驱动 `GameInput`：**真实设备**（后端轮询）或**磁带**。
磁带整帧接管输入（含边沿），所以这条路径不经过 `input_step`。
磁带在跑的时候 `F5`（录制）与 `F6`（换一盘）都会被拒绝并 `WARN`，避免两个来源互相干扰。

## 3. 从手工操作生成脚本（F5 → F8 → F5）

1. `F5` 开始录制：当前游戏状态被写成 `build/replay_state.sav`（磁带的第一条）
2. 想调手感就正常玩；到关键节点按 `F8` 打一个断言标记（可多次）
3. 再按 `F5` 结束：定下磁带长度并**立即导出** `build/replay_script.txt`
4. 用 `main.exe input_script build/replay_script.txt` 重跑；要长期保留就把脚本改名，
   并把第一行的 `save` 路径一起改成对应的存档（那两行是唯一的关联处）
5. 想立刻看回放：按 `F6`（它循环播放本次会话录的那盘）

`F4` 仍然可以随时再导出一份（例如回放途中用 `F8` 补了断言），导出的永远是**当前内存里那盘磁带**。

导出内容 = 第一行 `save <存档路径>` + 输入变化点（写成 `press`/`release`）+ 断言操作。
`F8` 打的标记写出四条断言：`assert_pos` / `assert_state` / `assert_grounded` / `assert_world` ——
「玩家状态 + 世界状态」，便宜、稳定，足够卡住绝大多数回归；其余断言需要时手写。

## 4. 造「从中途场景开始」的用例（`save_state <路径>`）

不想从出生点跑、又不方便手动录制时，让脚本自己在中途写一份存档：

1. 在脚本的某一帧写一行 `save_state build/checkpoint.sav`
2. 后面的某一帧写 `load_state build/checkpoint.sav` 回到那一刻（**同一帧内先 `load_state` 再断言，
   看到的就是存档点那一刻的状态**）
3. 于是「读档前的状态」与「读档后的状态」可以在同一段脚本里对比 —— 不必再维护“写/读”两个文件

`test/save_roundtrip.txt` 就是最小例子：跑 20 帧 → `save_state` → 再跑 20 帧（断言位置已经差 200px 以上）
→ `load_state` → 断言位置回到存档点（容差 0.05px）。

脚本也能引用**别处**（F5 录制、或别的脚本）生成的存档：把第一行写成 `save <那个路径>` 即可。

注意：**关卡一改，旧存档就会被指纹校验拒载**（表现为 `load_state` 失败）。这是想要的行为 ——
报错并提示重新生成，而不是让几十条断言在后面逐个失真。

验证「读档恢复」的思路：断言「离散状态一致」之外，还要断言「恢复之后的演化正确」——
例如二段跳能加上一段上升（`assert_air_jumps` + `assert_rise`），或者恢复后继续跑几十帧位移正常。
只对静态字段照抄是**测不出**漏存字段的。

同一条思路可以切成多段脚本：每段的第一行指向它自己那份存档，于是长流程可以拆成
「走到 A → 存 A」「从 A 走到 B → 存 B」……每段都能单独重跑与定位。

## 5. 帧号

帧号 = **磁带逻辑步**序号（60Hz），从 0 开始。它与装配层的 `logic_step` 是两条独立的时间轴：
磁带帧号永远从 0 开始，所以「什么时候按 F6」不影响脚本里的帧号。

- 输入行（`press`/`release`）的帧号**必须非递减**：磁带用「某帧起状态变成这样」表达输入，
  插入一个更早的帧就要重算它后面所有变化点的状态 —— 解析器会直接报错
  （旧实现是「写小了就顺延到当前帧静默执行」，那是个坑，已去掉）
- `press` / `release` 在该逻辑步的 `game_update` **之前**生效
- 断言与探针在该逻辑步的 `game_update` **之后**求值
- 断言行可以乱序书写（引擎按帧号排好）；同帧可以写多条

因为逻辑以固定步推进、输入来自磁带，同一份磁带每次运行结果完全一致（这也是它能当回归测试用的原因）。

## 6. 指令

| 指令 | 参数 | 说明 |
| --- | --- | --- |
| `save <路径.sav>` | | **必须是第一条非注释语句**：先读这个存档，再开始跑 |
| `save spawn` | | 同上，但不读档：从关卡出生点开始 |
| `press <动作>` | 动作名 | 该逻辑步按下 |
| `release <动作>` | 动作名 | 该逻辑步松开 |
| `mouse_move <x> <y>` | 客户区像素 | 鼠标位置（UI 靠它做悬停/命中；两个后端都已统一到客户区） |
| `mouse_press [按钮]` | `left`/`middle`/`right`（缺省 left） | 鼠标按钮按下 |
| `mouse_release [按钮]` | 同上 | 鼠标按钮松开 |
| `assert_pos <x> <y> [容差]` | 世界像素 | 校验角色中心坐标（容差默认 2.0） |
| `assert_state <状态>` | `IDLE`/`RUN`/`JUMP`/`FALL`/`DASH` | 校验动作状态 |
| `assert_grounded [0\|1]` | | 校验是否有支撑（缺省 1，即“必须在地面上”） |
| `assert_world <世界>` | `FIRST`/`SECOND` | 校验当前世界 |
| `assert_transition [0\|1]` | | 校验是否处于普通门的锁定输入、无敌自动走入阶段 |
| `assert_time_stop [0\|1]` | | 校验伪时停范围是否激活 |
| `assert_monster [0\|1]` | | 校验当前世界怪物是否存活（缺省 1） |
| `assert_monster_time_slowed [0\|1]` | | 校验怪物当前是否被伪时停覆盖（缺省 1） |
| `probe_monster_respawns` | | 记录当前世界怪物的刷新次数，作为后续刷新断言的基准 |
| `assert_monster_respawns <次数>` | | 校验自上次 `probe_monster_respawns` 后怪物新增的刷新次数 |
| `assert_monster_respawns_total <次数>` | | 校验当前世界怪物自进入该世界后累计的刷新次数 |
| `assert_projectiles <数量>` | | 校验当前飞行中的能量波数量 |
| `assert_air_jumps <次数>` | | 校验二段跳剩余次数（判断「跳跃能力」是否被正确恢复） |
| `probe_reset` | | 重置位移探针（记录基准点并清零极值） |
| `assert_rise <最小高度> [容差]` | 像素 | 探针区间内最大上升高度是否达标（**没先 `probe_reset` 算失败**） |
| `assert_run_x <最小位移> [容差]` | 像素 | 探针区间内最大水平位移是否达标（同上） |
| `save_state <路径.sav>` | | 在该帧写一份存档（造中途起点用例用） |
| `load_state <路径.sav>` | | 把世界恢复到那份存档的那一刻（输入继续由磁带驱动）。读/写失败会当场停掉磁带并以退出码 1 结束 |
| `log_state` | | 打印一行状态（观察用，不计入断言） |

动作名：`LEFT` `RIGHT` `UP` `DOWN` `JUMP` `DASH` `SHOOT` `TIME_STOP` `COIN` `ZOOM` `MAP`

`#` 开头是注释；**参数之后的 `#` 也会截断该行**（因此可以写行内注释）；空行忽略；带 BOM 的 UTF-8 文件也能正常解析。

## 7. 例

```
save spawn

# 原地起跳：校验上升/下落状态、落地与跳跃顶点高度
12    probe_reset
12    press   JUMP
18    release JUMP
30    assert_state    JUMP        # 上升中
40    assert_state    FALL        # 已过顶点
55    assert_grounded 1          # 已落地
55    assert_rise     220  8     # 顶点高度 ≥ 212 像素
```

## 8. 已知限制

- 帧号与坐标都写死：关卡布局或运动数值改动后，旧脚本的帧号与 `assert_pos` 坐标都要重新取样
  （`F7` 取样当前帧与状态；容差可先放宽确认趋势，再收紧到能卡住回归的数值）
- **关卡一改，旧存档就会被拒载**（存档带关卡几何指纹）。这是想要的行为：报错并提示重新生成，
  而不是让断言在几十帧后才失败。重新生成的办法见第 4 节
- 探针只有一个，同一时间只能验证一个区间极值
- 录制开始时若按着键（例如边跑边按 F5），文本只能表达「第 0 帧按下」—— 跳跃/冲刺会被重新触发一次；
  程序在 `F5` 时会 `WARN` 提醒，要精确对拍就先松手再开始录
- 回放刚载入、还没走完一个逻辑步时按 `F8` 会被忽略（没有「第 0 步之前」可断言）
- 文本能表达动作与鼠标（位置 + 按键）；**摇杆与扳机仍然只能留在磁带里**（回放能带），
  导出成文本时会 `WARN` 说明被丢弃
- 鼠标坐标是**客户区像素**，所以它和窗口尺寸/显示器分辨率绑定：把用例跑在别的窗口尺寸下要重新取样
  （例：`test/ui_map.txt` 里那几组坐标是按默认全屏 3840x2160 算的）
- **断言在 `game_update` 之后求值**，所以「第 0 帧」看到的位置已经走过一步（例如 vy=1150 时约 +18px）——
  写「恢复后的位置断言」时要按走一步后的值填，或者只断言那些一步内不变的状态
- 导出不覆盖基线回归脚本：导出到 `build/replay_script.txt`，用 `input_script build/replay_script.txt` 运行

## 9. 失败时能看到什么

目标是「不用猜」：失败信息要么指向文本的某一行，要么给出能自己解释「为什么」的状态。

### 9.1 解析期（还没开始跑）

所有解析错误都带行号，格式统一是 `input script: line <N>: <原因>`，并且**直接以退出码 1 收尾**
（不会静默掉进手动游玩）。实际会被拦下来的：

| 写法 | 报什么 |
| --- | --- |
| 第一条不是 `save` | `the first statement must be \`save <path.sav>\` (or \`save spawn\`)` |
| `save` 的存档不存在 | ``the save point `x.sav` does not exist``（存档点是在第 0 步之前读的，它必须现在就在） |
| `press`/`release` 帧号变小 | `frame 446 goes backwards (the last input frame is 449)` |
| 帧号是负数 | `the frame number must not be negative`（`strtoull` 会把 `-5` 回绕成巨大的数） |
| 必填参数缺失/不是数字 | ``\`assert_pos\` needs a value (\`y\`)``、``\`assert_rise min_rise\`: 'abc' is not a number`` |
| `load_state` 引用没人写过的文件 | ``\`load_state x.sav\`: nothing writes that file (add a \`save_state\` first, or fix the path)`` |
| 未知指令 / 未知动作、状态、世界名 | `unknown command 'assert_pso'` 之类 |
| 参数之后还有东西 | `WARN ... extra arguments after \`assert_grounded\` are ignored`（不中断，但一眼能看出打错了） |

必填参数这一点是刻意严格的：缺参数时旧实现会静默用 0，于是 `assert_rise` 变成「上升 ≥ 0」
—— 一个永远通过、却什么都没验的假断言。空断言比失败更难发现。

### 9.2 运行期（断言不成立）

每条失败是**三行**，紧接着是跑到最后的失败清单：

```
[ERROR] [tape] frame 60  FAIL assert_pos (9999.0, 9999.0) tol 2.0, actual (288.0, -1153.3)  [script line 3]
[ERROR] [tape]        context: pos (288.0, -1153.3)  vel (0.0, 0.0)  state IDLE  grounded 1  world FIRST  air_jumps 1
                                   coyote 0.10  dash 0.00/0.00  transition 0  time_stop 0  monster {active 0 …}  projectiles 0
[ERROR] [tape]        input: last change at frame 30: JUMP down
…
[DEBUG] TAPE: FAIL  (0/3 asserts passed, 151 logic frames)
[DEBUG] TAPE: 3 failed assertion(s), first 3 listed:
[DEBUG] TAPE:   line 3   frame 60   assert_pos
[DEBUG] TAPE:   line 4   frame 90   assert_rise
[DEBUG] TAPE:   line 6   frame 150  assert_run_x
```

- 第一行是「差多少」，`[script line N]` 直接指回脚本文本（录制产物没有来源行，就不写这一段）
- `context:` 给出**所有断言看得到的量**（位置/速度/状态/支撑/世界/二段跳/土狼/冲刺/过渡/时停/怪物/能量波）。
  失败原因通常就在这一行里 —— 比如 `vel (0.0, 0.0)` 说明角色根本没动，`world FIRST` 说明该跨图却没跨
- `input:` 给出这条断言之前**最后一个输入变化点**：回答「我明明按了跳」这类问题
  （帧号写错时，这里会显示变化其实发生在很久以前）
- 最后的 `TAPE:` 清单让「40 多条失败里哪几条是自己的问题」变成一次 grep；
  `TAPE:   line 4  frame 90  assert_rise` 这样的行可以直接跳到文本那一行改
- 磁带**中途停掉**（`save_state`/`load_state` 失败）时形状一致：
  `TAPE: FAIL  (stopped at frame 10: \`save_state\` could not be performed; 2 op(s) not evaluated, including this one)`
- 一条断言都没有的脚本会 `PASS` 并 `WARN`：`this tape has no assertions at all — PASS only means it ran to the end`

日志级别：`ERROR`/`WARN` 立即落盘，`DEBUG`（含每条断言的 OK 行与上面的 `TAPE:` 清单）走缓冲、
在正常退出与 `log_flush()` 时写出。**`game.log` 在每次进程启动时被清空**，所以它只含本次运行 —— 
排查时不需要按时间戳区分历史。

跑回归时这些细节由 `test\run_tests.bat` 收尾：用例失败会打印复现命令、前 12 行失败信息
（含 context 与行号）与 `TAPE:` 清单；确定性守卫失败时会用 `fc /l /n` 写出
`build\_trace_diff.txt`（按行号，第一列就是逻辑步帧号）并打印前 8 行。
