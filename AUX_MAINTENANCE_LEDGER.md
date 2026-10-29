# librime AUX 集成维护总记录

> 当前实现以官方 librime 1.17 为基线，采用独立 AUX 文件模型。v4–v9 章节保留作历史审计，不能作为当前配置说明；其中把 AUX 写入主词典行、`import_aux`、顶层 `aux/algebra` 或 `speller/auxiliary_separator` 的做法均已废止。

## 项目基线与文件身份

- 初始基线：本对话上传的 `librime.zip`（原始完整仓库快照），而不是推测 GitHub HEAD。
- v3 基线：此前已交付的 `librime-aux-phonetic-table-v3.zip` 修改文件合集。
- v4：在 v3 之上针对 `import_tables`、无表头子词典、主 Prism AUX、识别语义和诊断日志进行集成改造。
- v4 的源码包是**独立、完整的源码归档**，移除了原 zip 中的旧 `build/` 编译产物，避免混入旧二进制。
- 提供原始版→v4 总补丁，以及 v3→v4 增量补丁，两者二选一，不要重复应用。
- 上传初始文件不是用户电脑此刻的实时工作树；本地手工改动不在上传文件中，则不可能自动纳入。本包记录已知修复，但不能擅自视为同步。

## 语义基线（不可回归）

1. AUX 只作**硬约束**，不增加任何词条或句子的权重。
2. 不能把 AUX 的消耗长度限制为一位或两位；完全由 `aux` algebra 规则及 Prism 实际命中确定。
3. `abcde` 可以保留 `abc(AUX)+de` 和 `ab+cde` 两条解释；AUX 命中不自动吞掉后继声母。
4. 单字 AUX 的允许字符来自独立 AUX 文件，主词典只提供同一汉字的纯发音编码作为对齐依据。
5. 主 Table 只写入主词典的纯发音和词组编码；AUX 只写入 Prism 的派生键。
6. ReverseDB 保留普通反查和构词反查的原有接口；AUX 以独立命名空间保存，通过新增 `LookupAuxiliary(text, result)` 查询，不自动并入 comment。
7. 所有 AUX 路径准入过滤先于 `max_homophones`、WordGraph 和 Poet 剪枝；同时过滤系统词及用户词路径。
8. 无 AUX 的输入仍走普通 Rime 图，`preedit/comment` 必须继续回归验证。

## 当前实现（独立 AUX 文件模型）

- `dict/dict_compiler.cc`：从编译 schema 的 `translator/aux` 读取辅助码文件名，在用户/共享资源路径中依次查找 `.txt`、`.yaml`、`.dict.yaml` 或无扩展名文件；文件可以只有 `字<TAB>辅助码` 数据行，也可以带旧式 YAML 头。
- `dict/dict_compiler.cc`：主词典只编译 `import_tables`。`translator/aux` 只提供 AUX 文件路径；只有配置了 `speller/aux_algebra` 时，才会读取并投影 AUX 行，再把每个单字的普通 Prism 键和 AUX 变体合并，并保存允许的汉字集合。
- `dict/entry_collector.{h,cc}`：主表只处理纯发音和词组编码；`dict/reverse_lookup_dictionary.{h,cc}`：保留普通反查与构词反查，并把 AUX 投影结果写入独立反查命名空间，提供 `LookupAuxiliary()`，不改变现有接口返回值。
- `gear/script_translator.{h,cc}`、`algo/syllabifier.cc`：沿用 Prism AUX 元数据进行硬过滤，候选必须与对应汉字位置匹配。

配置入口：

```yaml
translator:
  dictionary: wanxiang_pro
  aux: aux                 # 查找用户目录中的 aux.txt / aux.yaml / aux.dict.yaml

speller:
  algebra:
    __patch:
      - wanxiang_algebra:/base/自然码
  aux_algebra:
    - "aux|^([a-z])([a-z])$|$1|"
    - "aux|^([a-z])([a-z])$|$1$2|"
    - "aux|^([a-z])([a-z])$|$1$2/|"
```

`aux.txt` 的内容只需是：

```text
啊	kk
阿	ek
锕	jk
```

因此不再需要 `auxiliary_separator: ";"`，也不需要在主词典中写 `拼音;辅助码`。例如主键 `hk` 和 AUX `nz` 会在 Prism 中生成 `hkn`、`hknz`、`hknz/`，Table 仍只保存 `hk`。

`translator/aux` 只指定 AUX 数据文件；真正的启用条件是存在 `speller/aux_algebra`。未配置规则时，AUX 文件中的原始编码完全无效，不会合流。若要保留原始编码，必须在 `aux_algebra` 中写出完整匹配的原样规则；辅助码长度不设限制。

## 早期 v4 改动（历史记录）

- `dict/dict_settings.{h,cc}`：`LoadDictHeader(stream, true)` 支持无表头导入表，保留所有数据行；主词典仍要求 name/version。
- `dict/dict_compiler.cc`：检测**实际 schema** 的 `speller/auxiliary_algebra`，而不是从 `speller/algebra` 或词典表头取 AUX 配置；默认源分隔符 `;`，非默认可在 schema 写 `speller/auxiliary_separator`。按导入列表编译 AUX；schema 更新也触发主 Table 更新；追加 `[aux-compile]` 摘要日志。
- `dict/entry_collector.{h,cc}`：接受 schema 上的 AUX 转写规则。对导入的**单个汉字**验证它的原始编码在规则下确实能得到 AUX 属性，才剥离为纯拼音并保留源编码反查。普通词条不做 AUX 处理。
- `gear/script_translator.{h,cc}`：不用额外的 `translator/auxiliary_dictionary`。直接使用**同一个主词典**的 Prism AUX 数据；默认有 AUX 编译产物即启用，可通过 `translator/auxiliary_mode: false` 或现有 `auxiliary_option` 关闭。`RIME_AUX_TRACE=1` 控制最多 300 条运行时详细日志。
- `algo/syllabifier.cc`：原生普通音节遍历跳过 AUX 专属描述，避免误当普通音节；AUX 仅由受约束路径 Provider 注入。
- `dict/prism.{h,cc}`：含 AUX 编译产物使用 3.2 版本标记；无 AUX 的词典仍为 3.1，不额外创建匹配器。
- `test/dictionary_test.cc`：新增无表头导入和依照实际 `aux` 运算决定是否剥离的用例；保留 v3 其他测试。

## 用户曾在本机执行的命令和结果（**历史记录**，并非该源码包内的改动）

- 命令：`python3 -c '...在 spelling.h 中补 Compose/Update 声明...'`；结果：修复了首个函数未声明错误，但后续暴露 `is_correction` 不属于 `SpellingProperties`。该方案不作为最终基线。
- 命令：`python3 -c '...移除 Compose/Update 声明并清空额外 spelling.cc...'`；结果：绕过 `spelling.cc` 不存在的成员引用。原始上传源码根本没有 `algo/spelling.cc`；本完整源码不附带这份人为追加的文件。
- 命令：`python3 -c '...删除 table.cc 中 if(false) {...ambiguous_source_positions...}...'`；结果：消除了即使 if(false) 仍会触发的成员编译错误。原始上传源码也没有这块代码，本完整源码保持原始 table.cc。
- 命令：`find . -type f -iname '*.yaml' ! -iname '*zi*' -exec perl -pi.bak -e '...编码第二列删除 ;[A-Za-z]{2}...' {} +`；目的：用户本机剥离非 zi 原始词库的辅助码。该命令作用于用户词库，并非 librime C++ 仓库；不在源码包内。它可能需要独立备份核对，尤其是行中多分号情况。
- 当前最后一次用户给出的构建日志在 `table.cc` 中停止，未看到完成的 librime 全量构建和万象回归。

## 历史配置示例（已废止）

### 主词典 `wanxiang.dict.yaml`（示例）

```yaml
---
name: wanxiang
version: "LTS"
sort: by_weight
import_tables:
  - dicts/zi
  - dicts/jichu
  - dicts/cuoyin
  - dicts/duoyin
...
```

### 导入表 `dicts/zi.dict.yaml`

仅正文，保留原始 `text<TAB>拼音;辅助<TAB>权重`：

```text
甲	ab;cd	100
家	ab;ef	9000
```

### `*.schema.yaml` 的新 AUX 配置

```yaml
speller:
  auxiliary_separator: ";"
  algebra:
    # 这里只写普通拼音/自然码规则
  auxiliary_algebra:
    # 规则输入只有分隔符后的辅助码，例如 nz
    - aux|^([a-z])([a-z])$|$1|
    - aux|^([a-z])([a-z])$|$1$2|
    - aux|^([a-z])([a-z])$|$1$2/|
translator:
  dictionary: wanxiang
  auxiliary_mode: true
```

编译器先用 `speller/algebra` 投影纯拼音，再把源行 `hǎo;nz` 在 `;` 处分成 `hǎo` 和 `nz`，只将 `nz` 交给 `speller/auxiliary_algebra`。上例会把 AUX 后缀投影为 `n`、`nz`、`nz/`，再与普通键 `hk` 合成 `hkn`、`hknz`、`hknz/`。辅助码规则不接收拼音，也不承担自然码转换；单字主 Table 只写入 `hǎo`，Prism 同时保存普通键和 AUX 键。分隔符始终位于 `speller/auxiliary_separator`，不能从导入词典表头或顶层 `auxiliary` 配置读取。

## 日志与核查

- AUX 编译和运行路径不再输出临时诊断日志；只保留配置错误、源文件缺失、规则解析失败和 AUX 数据无法映射到主音节等必要错误。
- 需要核查时，应直接检查生成的 Prism、ReverseDB 和运行行为，不依赖日志计数。
- 部署前备份 `*.userdb`、编译结果及当前工作树；重新部署需重建 table/prism/reverse，观察两条分词是否共存。
- 当前执行环境没有完整 C++ 依赖，尚未在这里成功编译整套 librime 或运行真实输入回归。**不可称为生产完成版**。

## 最低回归清单

- 无表头导入：第一行与后续行不丢失；表头配置无法从 zi 意外覆盖主词典。
- AUX 元数据命中：`abc` 与 `ab` 主音节对齐，输入 `abcde` 两种路径都保留。
- `abc` 没有合法字时，不应强制消费 `c`；普通分词继续可用。
- `max_homophones` 前做硬过滤，低频符合 AUX 的字仍能被检索。
- 多字词中间、词尾 AUX 能匹配相应字位；非匹配路径完全排除。
- 用户词典调频、UTF-8 字位、数字声调、comment、preedit、Lua、九键、简码回归。
- 关闭 AUX、移除规则后，普通词典的编译与输入行为不能退化。

## 下次继续的权威工作池

以本包为**可复现的源码基线**；下一次提供 `librime-aux-v4-full-source.zip` + 最新编译/运行日志即可定位；所有新改动先产生 v4→vN 补丁、测试与更新本记录，再交付。不要仅凭对话中的口头总结推测已修改的文件。

## v4.1 | 2026-10-28 | 修复 DictCompiler::BuildTable 六参数签名未同步

- **问题证据**：用户实际编译到 `dict_compiler.cc:217` 时调用 `BuildTable` 带 6 个参数，而 v4 的 `dict_compiler.h:42` 只声明 5 个；`dict_compiler.cc:287` 定义也有 6 个参数，导致编译中断。
- **根因**：v4 接入 `an<ConfigList> auxiliary_algebra` 后只更新了主 Table 的调用和 `.cc` 定义，漏掉 `.h` 声明，且词典 pack 的另一处调用还只传 5 参数。
- **最终源码更改**：`src/rime/dict/dict_compiler.h` 的 `BuildTable` 声明增加 `an<ConfigList> auxiliary_algebra`；`src/rime/dict/dict_compiler.cc` 中 pack 调用显式传入 `nullptr`（pack 不启用主 schema AUX 投影）。主 Table 调用仍传 `algebra`；函数定义不变。
- **影响范围**：修复编译接口一致性，无 AUX 命中、词库规则及评分语义的改变；保留主 Table 与 pack Table 的区别。
- **用户命令（建议、尚未在用户电脑执行）**：`git apply --check librime-aux-v4-to-v4.1.patch && git apply librime-aux-v4-to-v4.1.patch && make -j"$(nproc)" release`，前提是从 librime 根目录且文件符合 v4 基线。
- **回滚**：从仓库根目录执行 `git apply -R librime-aux-v4-to-v4.1.patch`，仅在未叠加后续更改时适用。
- **验证**：patch 用来自 v4 包的原始两个 C++ 文件通过 `git apply --check` 和正向/反向往返校验；静态核对声明、定义及两个调用点为六参数。完整 CMake 构建、链接、真实输入仍未完成。
- **补丁 SHA-256**：`d5c3a00d78f54e699e8f8285aa719862ad0a882fc4150c2d767e2e0168c356b6`。

## v4.1.1 | 2026-10-28 | 本机新增 OpenCC 源码的 UTF-8 路径接口兼容

- **用户实际编译错误**：`src/rime/gear/opencc.cc:27:50` 调用 `config_path_.to_utf8_string()`，但上传的 `rime::path` 类型没有该成员。
- **源码基线确认**：v4.1 归档含 `src/rime/gear/simplifier.cc`，**不含** `src/rime/gear/opencc.cc`；此路径应来自用户本机另外合入的文件/分支，不能将未知完整实现直接假设为 v4.1 代码的一部分。原版 `simplifier.cc` 在 OpenCC `NewFromFile()` 调用中使用 `config_path_.u8string()`。
- **根因**：`rime::path` 继承 `std::filesystem::path`，项目使用 C++17，继承了标准 `u8string()`，但不支持 `to_utf8_string()`。
- **本机建议改动（非已执行）**：只替换本机文件 `src/rime/gear/opencc.cc` 中一次 `config_path_.to_utf8_string()` → `config_path_.u8string()`；先备份为 `src/rime/gear/opencc.cc.aux-backup`。
- **纳入交付工作池的方法**：源码基线保持 v4.1 不变（**不会制造一个不存在的 `src/rime/gear/opencc.cc`**）；提供在真实本地源码上执行的幂等校验脚本 `tools/aux_compat/fix_local_opencc_path.py` 和命令记录。本次 v4.1.1 是**记录/兼容脚本版**，不是声称已合入用户本地源码的 C++ v4.1.1 新实现。
- **影响范围**：OpenCC 配置文件路径字符串的转换，和 AUX 硬过滤、Prism、Table、UserDictionary 评分完全无关。
- **验证**：已查验 v4.1 归档内 `CMAKE_CXX_STANDARD 17`、`rime::path` 继承声明、原 `simplifier.cc` 的 `u8string()` 实现；用户机器完整构建尚待结果，未知本机 `opencc.cc` 其他改动。
- **回滚**：先停止编译，恢复 `opencc.cc.aux-backup` 到 `opencc.cc`，或只将该一次替换还原。

## v7 | 2026-10-28 | 运行时回归与完整构建修复

- `src/rime/dict/dict_compiler.cc`：`Projection::Apply()` 返回 false 表示没有规则改写，不是失败。旧代码因此清空了纯音节脚本，AUX-only algebra 会让 Prism 只剩 AUX 键，普通 `ni`、`zl` 路径消失。现改为保留未改写的普通脚本。
- `src/rime/algo/syllabifier.cc`：AUX 匹配跨输入分隔符时，边的结束位置从 `begin_pos + length` 计算，和普通拼写边一致；清理阶段删除无法连接到完整输入的 AUX 约束，避免无效 AUX 前缀影响普通分词。
- `src/rime/gear/script_translator.cc`：AUX Prism 命中在进入图前确认对应纯音节有实际 Table 单字条目；没有 Table 条目时记录 trace 并跳过。
- `test/syllabifier_test.cc`：增加分隔符跨度和未完成 AUX 前缀回归测试。

验证证据：使用独立 `build_codex` 目录完成 CMake 配置、全量 librime/工具/GTest 编译；`ctest --test-dir build_codex --output-on-failure` 通过 156 个测试。另用最小词典部署运行时验证 `hkn → 好`、`niz → 逆`，并确认 `nizl` 保留普通 `ni + zl` 路径。原有 `build/` 因部分 CMake 文件属于 `nobody` 无法覆盖，未将其权限改动纳入源码修复。

## v8 | 2026-10-29 | AUX 独立转写入口与真实词库链路

- `src/rime/dict/dict_compiler.cc`：AUX 规则只从顶层 `aux/algebra` 读取。主 `speller/algebra` 永远只生成普通 Prism；独立 AUX 代数负责把 `拼音;辅助码` 源行投影到 Prism；规范 AUX 键再从普通音节投影加辅助后缀生成，避免把原始 `hǎo` 误编成 `haon` 而丢失 `hk+n`。
- `src/rime/dict/dict_compiler.h`：`BuildPrism` 接收独立 AUX 代数。
- `src/rime/algo/syllabifier.cc`：输入中的空格/分隔符只影响边的原始跨度，AUX 查找使用压缩后的键；因此 `hk n` 的查询键是 `hkn`，但图边仍覆盖输入 `[0,4)`。
- 真实万象词库部署结果：Table 中 `好` 只有 `hǎo`/`hào`，没有 `;nz`；Prism 同时含普通 `hk` 与规范 AUX `hkn`；编译日志统计 `raw AUX rows=51050`、`normalized_entries=119553`、`accepted_characters=189720`。
- 最小运行时回归：`hkn` 命中 `好`；`fjia` 的底层 Dictionary 查询同时保留单字 `反` 和词组 `反差`、`反查`、`翻查`，说明单字路径没有在硬过滤层丢失。最终候选页仍受菜单页大小及上层 Lua filter 排序影响，应在 Fcitx5 进程加载新库后复核显示顺序。

验证：`cmake --build build_codex -j2` 成功；AUX 相关 `RimeSyllabifierAuxiliaryTest` 5 项通过。真实编译产物位于临时目录 `/tmp/rime-real-build2`，未写入系统库或用户 Fcitx 数据目录。

## v9 | 2026-10-29 | 严格双代数模型（当前实现）

- schema 只从 `speller/auxiliary_algebra` 和 `speller/auxiliary_separator` 读取 AUX 配置；不再读取顶层 `aux`、`speller/auxiliary_algebra` 以外的旧入口，也不从源词典表头启用功能。
- 编译器把 `拼音;辅助码` 明确拆成两段：主 `speller/algebra` 只处理前段，`speller/auxiliary_algebra` 只处理后段。两段投影结果最后在 Prism 合流；辅助代数不再接收整行，也不承担自然码投影。
- `EntryCollector` 只有在独立 AUX 代数实际产生 AUX 属性时才剥离单字表编码；没有代数就保留原始源编码，避免词库数据自行改变编译策略。
- `aux` 规则即使替换结果与原字符串相同，也会登记为 AUX 变体，支持同时声明 `c`、`cd`、`cd/` 等显式后缀。
- `BuildTable`、`BuildPrism`、`EntryCollector::Configure` 的新调用链要求显式传入 AUX 参数；pack 表明确传入空代数，不依赖默认兼容参数。

验证：独立最小词典 `hǎo; nz` 编译后 Table 只有纯 `hk`，Prism 同时生成 `hk`、`hkn`、`hknz`；完整 `cmake --build build_codex -j2` 成功，CTest 158 项全部通过（含 1 项既有 disabled 测试）。
