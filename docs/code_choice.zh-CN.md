# Prism 公共编码选择（Code Choice）

## 设计原则

编码选择是 Prism 的原生编码收窄能力，与键盘布局、字符类型和编码长度无关。
不声明 keymap，不把数字翻译成字母，不通过去声调猜测拼写，也不维护影子输入。

设 S(k) 为编译后的编码 k 对应的原始 SyllableId 集合。
仅当 S(b) 是 S(a) 的非空严格子集时，b 才是 a 的一个可选编码。
相等集合不产生收窄关系；等价目标编码只保留字典序最小的代表。
相同源集合共享关联记录。混有简拼、纠错描述符的键不参与索引。
这不是物理键位关系：任何布局均遵循同一规则，允许不同长度及字符的编码。
例如 986 的音节集合包含 zuo 的全部读音，因此可提供 zuo；同理 xun、yun、zun。
仅有交集而不完全包含的目标不能选中，以免引入新音节。

## 部署算法

`DictCompiler::BuildPrism` 执行 algebra 得到 Script 后，调用 `Prism::Build`。
Script 的键按字典序排列，Darts Trie 的值与键序号（Spelling ID）对应。

1. 将每个合格编码的原始音节转换为整数 ID，排序去重，按集合哈希分组。
2. 对多元素源集合建立 SyllableId → 源集合编号的倒排表（密集 ID 直接寻址）。
3. 每个目标集合选择最短倒排表，筛选可能的超集，再检查基数和 std::includes。
   哈希只用于分组，最终集合包含关系精确验证，不依赖哈希碰撞概率。
4. 按 Script 顺序选择等价集合的代表，生成共享记录和字符串池。
5. 预估 mmap 文件空间后写入所有数据。临时分组、倒排表只在编译期存在。

无映射表、一对一或无严格子集的 Prism 不生成关联数据。普通词库、用户词、
音节图、候选排序及辅助码匹配仍走原有链路。

## Prism 7.0 二进制契约

Metadata 原有字段保持原位，尾部新增两个 List（共 16 字节）：
`List<uint32_t> code_choices` 和 `List<char> code_choice_strings`。
List 的 size 是元素数，at 为相对自身地址的 OffsetPtr。
两个 List 均为空表示没有关联，属于已编译的合法状态。

设 N = num_spellings。code_choices 的前 N 个 uint32_t 是密集索引；
0 表示无关联，非零值是同一区域内以 uint32_t 为单位的记录偏移。
记录布局为 [count, string_offset_0, ..., string_offset_(count-1)]。
字符串偏移以字节为单位，从 code_choice_strings 起点计算；字符串以 NUL 结束。
多个源编码可指向同一记录；重复出现的目标字符串只储存一次。
关联数据无独立文件、无运行时缓存、无锁、无反向 Trie 枚举。
关联区大小随有效关系数量而变，索引存在时额外使用 4*N 字节的直接寻址表。

旧格式仍可读取但无关联能力。部署检查 has_code_choice_format()，
只升级旧 Prism，不因格式升级重建 Table / Reverse。切换 Prism 自动切换关联区。

## C++ 接口

```cpp
bool HasCodeChoices() const;
prism::CodeChoiceView QueryCodeChoices(int spelling_id) const;
prism::CodeChoiceView QueryCodeChoices(std::string_view key) const;
```

返回借用的 mmap 只读视图；调用者可用 size()、empty()、operator[] 访问。
视图仅在所属 Prism 映射有效且未卸载/重新加载期间有效，不能跨部署保存。
传入 Spelling ID 时直接偏移寻址 O(1)；传入字符串时先做一次 Trie 精确查询，
再 O(1) 寻址；遍历返回的 k 个选项需要 O(k) 次访问及字符串长度的读取。

## 前端 C API 契约

```c
Bool (*get_code_choices)(RimeSessionId, RimeCodeChoice**, size_t*);
void (*free_code_choices)(RimeCodeChoice*, size_t);
Bool (*select_code_choice)(RimeSessionId, size_t start, size_t end,
                           const char* replacement);
```

使用 RIME_API_AVAILABLE 检查三个尾部接口。get 返回当前活动段最早未确定的
完整编码选项，start/end 为原始 UTF-8 输入字节区间（end 不包含）。
label 用于显示；replacement 是实际替换文本，可能包含用于保护切分的分隔符。
前端必须复制结果并调用 free，不能自行根据 label 推导 replacement。
选择时提交原快照的区间及 replacement，内核重新验证当前选项，
Context::ReplaceInput 保留已确认前缀及剩余输入、调整光标并只通知刷新一次。
选择不是上屏，不清空后续输入，不额外保存选中标签作为过滤条件。

脚本翻译器使用原生音节图确定可选择范围；形码翻译器查询完整编码。
不完整音节、歧义辅助码及 canonicalize 重排当前未提供选择能力。
索引只包含实际编入 Prism 的编码，不为 UserDB 独有编码推导新关系。

## 回归验证

test/prism_test.cc：严格子集、交集拒绝、等价集合、不同长度、共享源、
无关联、持久化读取以及旧格式读取；test/code_choice_test.cc：原始输入替换、
光标与尾部保留、通知次数。Android 需用 EVO SDK 的真实部署与 JNI 选择验证，
不能将旧 SDK 返回空选项视为测试通过。
