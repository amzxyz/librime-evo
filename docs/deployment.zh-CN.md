# 插件部署接口

EVO 支持插件参与统一的资源编译和部署生命周期，无需修改内核的部署代码。

## 1. 注册编译器

插件实现 `BuildCompiler`，在模块初始化时注册对应的 `build/<名称>`。例如纠错插件：

```cpp
#include <rime_api.h>
#include <rime/config.h>
#include <rime/deployer.h>
#include <rime/lever/build_compiler.h>
#include <rime/registry.h>

using namespace rime;

// 由插件实现实际的资源构建逻辑。
bool BuildCorrector(const path& table, const path& output);

class CorrectorCompiler : public BuildCompiler {
 public:
  explicit CorrectorCompiler(Deployer* deployer) : deployer_(deployer) {}

  bool Collect(const BuildRequest& request, BuildPlan* plan) override {
    string dictionary;
    if (!request.config->GetString("build/corrector/dictionary", &dictionary))
      return false;

    const string id = "corrector:" + dictionary;
    const string output = dictionary + ".corrector.bin";
    const path table = deployer_->staging_dir / (dictionary + ".table.bin");
    const path target = deployer_->staging_dir / output;

    // 任务 ID、预计峰值内存（字节）、编译函数。
    CompileUnit unit(id, 16 * 1024 * 1024, [table, target]() {
      return BuildCorrector(table, target);
    });
    unit.AddOutput(output);
    plan->AddUnit(std::move(unit), request.schema_id);
    plan->AddDependency("dictionary:" + dictionary, id);
    return true;
  }

 private:
  Deployer* deployer_;
};

static void rime_corrector_initialize() {
  Registry::instance().Register(
      "build/corrector", new Component<CorrectorCompiler>);
}

static void rime_corrector_finalize() {}

RIME_REGISTER_DEPLOYER_MODULE(corrector)
```

`RIME_REGISTER_DEPLOYER_MODULE` 使已链接或加载的插件也能在独立部署进程中初始化。已有自定义模块也可设置 `module->load_for_deployment = True`。部署模块的初始化不应依赖运行时 gear。

## 2. 方案配置

```yaml
build:
  corrector:
    dictionary: example_pinyin
```

部署时自动查找 `build/corrector`，收集任务并统一调度。本例依赖本次部署中的 `dictionary:example_pinyin` 任务，词库编译成功后才构建纠错资源。依赖与注册顺序无关；前置任务失败时，依赖任务自动跳过。

## 3. 编译接口

| 接口 | 作用 |
| --- | --- |
| `BuildCompiler::Collect()` | 读取配置并收集编译任务 |
| `BuildCompiler::Prepare()` | 编译前准备 |
| `BuildCompiler::Finish()` | 编译结束后清理 |
| `BuildPlan::AddUnit()` | 添加任务并记录所属方案 |
| `CompileUnit::AddOutput()` | 声明产物，同文件写入自动串行 |
| `BuildPlan::AddOwner()` | 为共享任务添加方案归属 |
| `BuildPlan::AddAlias()` | 为共享任务注册其他资源 ID |
| `BuildPlan::AddDependency()` | 声明任务依赖 |

每个编译命名空间在一次部署中只有一个编译器实例。上例仅展示单个资源的注册方式；多个方案共享资源时，应由编译器合并同名同定义的任务，并记录各方案归属。编译器自行处理输入校验、增量判断和原子写入。`BuildRequest` 仅在 `Collect()` 调用期间有效，任务应按值保存必要数据。

## 4. 部署通知

仅需要感知部署过程的插件，也可以订阅 `Deployer` 的同步通知，不必注册编译器。

```cpp
#include <rime/service.h>
#include <rime/deployer.h>

auto& deployer = rime::Service::instance().deployer();

// 在模块初始化时连接，并保存 connection；在模块卸载时 disconnect。
auto before = deployer.before_deploy().connect(
    [](rime::Deployer*, const rime::vector<rime::string>& schema_ids) {
      // 方案配置编译前，准备源文件或配置。
    });

auto after = deployer.after_deploy().connect(
    [](rime::Deployer*, const rime::DeploymentResult& result) {
      // 全部编译任务和清理结束后，检查部署结果。
      // result.success
      // result.successful_schemas
      // result.failed_schemas
    });
```

通知按部署批次触发，而非逐个方案触发。不要在通知中重入部署、保留事件参数引用或执行应纳入调度的资源编译。

接口定义：[BuildCompiler](../src/rime/lever/build_compiler.h)、[CompileUnit](../src/rime/lever/compile_unit.h)、[Deployer](../src/rime/deployer.h)。[English documentation](deployment.md).

## 前端部署结果与预热

维护线程结束后，前端可调用 `get_deployment_result` 读取本轮方案部署结果，并使用 `free_deployment_result` 释放。未执行方案部署、选择阶段失败或结果尚未就绪时返回 false；方案编译成功并不等同于运行时资源完整。只有当前方案明确成功、其他方案明确失败且没有独立维护任务失败（`deployment_has_other_failure`）时，前端才可将整体失败降级为部分失败提示。

`get_initial_schema` 使用 Switcher 的真实初始方案选择规则，不需要创建会话。空闲时调用 `prewarm_schema(nullptr, &paths)`，通过原生资源工厂准备该方案实际使用的只读词典、Prism、辅助码、Rewrite 和语言模型。返回的是实际映射文件的绝对路径，可交给有内存预算、可取消的后台 I/O 预读；使用 `free_string_list` 释放路径。预热是可选优化，不启动部署、不创建会话、不打开 UserDB，不承担运行时完整性认证。只保留一个预热方案的资源，跨输入会话复用，直至 `release_prewarm`、下次部署前或 finalize。调用预热与会话操作应在同一引擎所有者线程串行完成；活跃会话时不重复创建预热对象。
