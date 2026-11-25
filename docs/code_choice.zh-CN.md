# Prism 公共编码选择

编码选择是 Prism 的原生能力，与键盘布局、编码字符和编码长度无关，
无需额外配置。目标编码的原始音节 ID 集合必须是源编码的严格子集。
相等集合去重，混有简拼或纠错的拼写不参与索引。

## 编译与存储

部署时在 Prism::Build 中直接使用 Script，按音节集合分组并通过
倒排索引筛选候选超集，精确验证严格子集。不反查 Trie。
Prism 7.0 在原有 Metadata 尾部增加两个可空的数据区：
- uint32_t 索引及记录：前 N 项由 Spelling ID 直接寻址，0 表示无关联；
  后续记录为选项个数和字符串偏移。同一集合的源编码共享记录。
- 字符串池：确定编码仅存储一次。

不存在关联时两个区均为空，不分配索引。旧 Prism 可读取，但部署会
自动升级，不因为格式升级而重建 Table 或 Reverse。

## 接口

Prism::HasCodeChoices() 判断是否存在关联；
Prism::QueryCodeChoices(spelling_id 或编码字符串) 返回只读视图，
在 Prism 映射有效期内使用。没有哈希缓存、初始化锁和运行时子集计算。

前端 C API 保留 get_code_choices / free_code_choices / select_code_choice。
前端使用当前原始输入字节范围及 replacement；选择时直接替换 Context，
脚本翻译器使用分隔符保护音节边界，不维护影子输入。
不完整编码、歧义辅助码及 canonicalize 重排暂不提供选项。
