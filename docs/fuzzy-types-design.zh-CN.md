# 弱拼写与运行时模糊音

## 拼写运算

原 `fuzz` 改名为 `weak`，原有类型数值、降权、分词路径保护及 strict_spelling 行为不变。旧 `fuzz` 和 `derive/.../fuzz` 不再支持。新 `fuzzy` 是有启用条件的派生运算，不增加权重惩罚，也不改变原有 SpellingType。

```yaml
- weak/^([a-z]).+([a-z])$/$1$2/
- abbrev/^([a-z]).+$/$1/
- fuzzy/^zh/z/z_zh
- fuzzy/^z(?!h)/zh/z_zh
- fuzzy/an$/ang/an_ang
- fuzzy/ang$/an/an_ang
```

模糊组 ID 由方案定义，内核不预设任何编码或语音关系。规则与普通转写规则一样按声明顺序执行；重叠规则不自动计算传递闭包。条件派生只能用于词典 Prism 编译，不用于只转换单个字符串的运行时 Projection。

## 方案声明

```yaml
runtime_settings:
  fuzzy_types:
    mode: multiple
    default: []
    values:
      z_zh: "z / zh"
      an_ang: "an / ang"
build:
  $example:
    type: dictionary
    fuzzy: ${fuzzy_types}
    prism:
      - fuzzy/^zh/z/z_zh
      - fuzzy/^z(?!h)/zh/z_zh
translator:
  dictionary: $example
  fuzzy: ${fuzzy_types}
```

最多 16 组；ID 唯一，规则引用的组必须已声明。label 只用于显示。具体规则由各方案自行声明，可参考 [拼音示例](fuzzy-pinyin.example.yaml)。

变量名由方案自由选择；本例使用 `fuzzy_types` 作为多选变量，默认全部关闭。普通 `switches` 的配置形式不变。自定义布尔、单选、多选可使用 `runtime_settings`；统一 API 参见 [前端接口](frontend-api.zh-CN.md)。

## Prism 存储

Prism 8.0 使用原来的 32 位描述符字段，现命名为 flags：

| 位 | 用途 |
| --- | --- |
| 0–3 | SpellingType |
| 4–19 | 模糊组需求 |
| 20–29 | 预留 |
| 30 | 纠错标记 |
| 31 | 预留 |

每个 Prism 按实际规则首次出现顺序分配 bit，并保存一份 ID 对照表；每条映射不重复保存组名。旧格式需要重新部署。单条描述符长度不变，但新增派生映射、组名会增加整体文件大小。

同一路径累加必需组；独立派生路径分别存储。仅当一个路径所需组是另一路径的子集且其他属性不劣时，才可去掉被支配路径。输入 zang 的示例：

| 原音节 | 所需组 |
| --- | --- |
| zang | 无 |
| zhang | z_zh |
| zan | an_ang |
| zhan | z_zh、an_ang |

运行时 `(required & enabled) == required` 才放行。无条件映射始终有效。同一音节的有效路径在读取时选择优先属性，避免重复输出。没有条件映射的 Prism 仍使用普通遍历路径。

## 持久化与运行时

`get_setting` / `select_setting` 获取或更换值列表。内核在 `user.yaml` 的 `var/type_groups/<schema_id>/fuzzy_types` 保存稳定 ID，不保存 bit 编号。显式空列表表示全部关闭。组删除或更名后，旧 ID 不再生效；重排列或改变显示名称不会改变用户选择。

运行时按当前词典的 Prism 把 ID 映射为启用掩码，映射筛选只需要位运算。选择状态不写入共享 Prism。切换设置后刷新当前会话未确认部分，不重新部署或修改方案配置。

普通精确查词、补全、分词及辅助码匹配在读取映射时过滤。音节选择仍基于原生无条件映射的编译期严格子集索引；模糊音仅参与候选查询，不改变音节选择器的索引、选择协议及运行时算法。

## 验证范围

应覆盖独立组、组合组、替代路径、正常映射覆盖、简拼/纠错叠加、天然同码、补全、直接查词、辅助码、旧格式重新部署、空选择持久化、方案隔离及九键严格子集关系。磁盘写入沿用配置保存机制；整组值校验与替换不等于文件系统事务。
