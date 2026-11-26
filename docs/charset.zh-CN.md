# 原生字符集准入

通过 `build/rewrites` 编译普通 UTF-8 TSV 键值数据（单字、TAB、字符集属性字符串），
以命名空间配置字符集策略。无需注册字符集滤镜。

```yaml
build:
  rewrites:
    charset:
      files: [data/charset.txt]

translator:
  charset: charset

charset:
  data: charset
  exclude_tags: [reverse_lookup, wanxiang_reverse]
  rules:
    - option: charset_filter
      base: a
      addlist: ["诶濛𰻞"]
      blacklist: []
    - options: [s2t, s2hk]
      base: fa
```

`script_translator` 与 `table_translator` 均可通过各自的 `charset` 字段引用策略。
未配置的翻译器不受影响。保留原有 `charset_filter` 和
`enable_charset_filter` 行为供其他方案使用；码表翻译器配置新策略时以新策略为准。

启用规则之间的 base 和 addlist 取并集，blacklist 一票否决；
`option: true` 表示始终启用。属性区分大小写。无规则开启、命中 exclude_tags
或资源不可用时放行。

只过滤系统词典中恰好一个汉字的词条；多字词、用户词、非汉字不检查。
组句词图在同音词截取和语言模型评分前完成单字准入，不需要候选历史兜底。
原生查询与 Lua RewriteLookup 共用 RewriteResource，数据格式不变。
选项变化通过引擎已有的重组流程刷新候选。

迁移旧 Lua 字符集组件时，应先部署完整 TSV 并确认新策略生效，再移除 Lua 滤镜；
如其他 Lua 组件还使用其 make_checker，应单独迁移这些调用。
