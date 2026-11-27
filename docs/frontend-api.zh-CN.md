# 前端接入与扩展 API

公共接口以 [rime_api.h](../src/rime_api.h) 为准。前端负责 UI、输入事件及设备资源调度，内核负责候选操作、方案语义、部署和资源解析。

## 配置职责

| 配置 | 当前接入方式 |
| --- | --- |
| `switches` | 传统配置 API 读取菜单定义，get_option 读取运行时状态 |
| `switcher/save_options` | 已废弃。普通开关通过 select_switch 按方案记忆，set_option 仅改变运行时状态 |
| `switcher/fix_schema_list_order` | 传统配置；下一会话的实际初始方案通过 get_initial_schema 获取 |
| `translator/dictionary`、`translator/prism`、`prisms`、`build/dictionaries` | 内核资源工厂解释；前端不推导编译文件名 |
| `grammar/language` | 模型插件解析；预热返回实际映射文件路径 |
| `engine/processors`、`super_sequence` | 候选编辑通过 edit_candidate 接口判断可用性并调用，不解析 Lua 快捷键。不是通用组件状态查询 |
| `input_types`、`aux_types` | 使用 get_type_options / select_type_option，无需解析配置组 |

配置编辑器读写 YAML 不属于运行时语义推导。

## 扩展接口索引

| 接口 | 用途 |
| --- | --- |
| get_candidate_preview / free_candidate_preview | 当前高亮候选的提交预览；初始化结果结构并成对释放 |
| get_code_choices / free_code_choices | 枚举首个未确认编码的选项；数组由内核分配，start/end 为原始输入字节偏移，end 不包含 |
| select_code_choice | 校验当前有效范围并选择编码 |
| get_type_option | 获取指定方案、类型组的当前或已保存选择 |
| get_type_options / free_type_options | 枚举 option、label、selected，保持方案定义顺序 |
| select_type_option | 选择并保存独立的方案类型组；可在无活动会话时保存 |
| edit_candidate | 直接处理 auto_delete、manual_delete、manual_up、manual_down；index 可为全局或当前页索引；返回已处理不等于数据库实际发生变化 |
| get_deployment_result / free_deployment_result | 获取并释放最近方案部署结果；不是运行时完整性认证 |
| deployment_has_other_failure | 独立维护任务失败或结果未知时返回 true |
| get_initial_schema | 获取下次会话的初始方案，无需创建会话 |
| prewarm_schema / free_string_list | 无活动会话时准备只读资源，返回实际映射路径；空方案 ID 使用初始方案。路径列表由内核分配并成对释放 |
| release_prewarm | 释放临时资源持有；不强制回收操作系统页缓存 |
| select_switch | 主动选择普通开关；reset 不保存，无 reset 按方案保存；单选组选择 true，false 不取消整组；未声明的选项返回 false |

预热及部署细节见 [部署文档](deployment.zh-CN.md)，编码选择见 [编码选择文档](code_choice.zh-CN.md)。

## 公共 C ABI 长期约束

- 允许扩展 API，原则上不得破坏现有公共 C ABI。
- 新函数追加到 RimeApi 末尾，不改变既有函数槽位、签名或结构体布局；废弃接口保留槽位。
- 扩展数据优先使用独立结构体及配套释放函数；不得扩大已有非版本化结构体。
- 前端编译时使用对应特性宏，运行时使用 RIME_API_AVAILABLE 检查函数可用性；不得仅凭引擎名称认定功能。
- 内核重构、上游同步、第三方代码移植及发布前检查公共 C ABI。确需破坏时先说明原因、影响与迁移方案。
- 公共 C ABI 兼容不代表内部 C++ 插件 ABI 或数据格式兼容。

## 普通开关记忆

`switcher/save_options` 已废弃，普通开关由方案的 `switches` 定义。初始化时有效 reset 优先，否则恢复 `var/schema_options/<schema_id>/<option>`；缺少记忆时布尔默认 false，单选组默认第一项。不读取旧的全局 `var/option` 数据。单选组整组保存。临时 set_option、快捷键及 inline ASCII 不触发记忆。前端应在用户明确选择时调用 select_switch；类型组仍使用独立接口。不同方案切换时清理旧方案普通开关并在构建组件前恢复目标方案。

Lua 插件的 `Switcher:select_switch` 与 C API `select_switch` 使用相同的内核持久化规则。
