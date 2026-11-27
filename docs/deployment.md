# Deployment extension API

A plugin that supplies deployment work can opt in when registering its
module. A deployment-capable module is initialized once, also when running
the standalone deployer. Runtime-only modules retain their usual behavior.

```cpp
#include <rime_api.h>
#include <rime/lever/build_compiler.h>
#include <rime/registry.h>

static void rime_corrector_initialize() {
  rime::Registry::instance().Register(
      "build/corrector", new rime::Component<CorrectorCompiler>);
}

static void rime_corrector_finalize() {}

RIME_REGISTER_DEPLOYER_MODULE(corrector)
```

The module must be linked or loaded by the host. For an existing custom
module, set `module->load_for_deployment = True` in its customization
function. A deployment-capable initializer must not rely on runtime gears.

## Resource compilation

Declare resources under `build/corrector` in a schema and implement
`BuildCompiler::Collect`. There is one compiler instance per section for
the whole batch. Copy any configuration needed by a compile unit; the
request is only valid during Collect.

Register each unit with a unique ID, estimated peak memory, outputs and
the owning schema. To share a unit across schemas, add each owner.
For example, a corrector unit can depend on `dictionary:wanxiang`:

```cpp
plan->AddDependency("dictionary:wanxiang", "corrector:wanxiang");
```

Explicit dependencies are resolved after collection, independent of
declaration order. If multiple resources share one compile unit, register
each resource ID using `BuildPlan::AddAlias`. A failed prerequisite blocks its dependents, while
units merely writing the same output are serialized. A cycle or missing
unit ID invalidates the plan. The compiler is responsible for validating
its inputs, detecting unchanged data and installing outputs atomically.

`Prepare` and `Finish` run on the deployment caller's thread; unit work
may execute on workers. Do not write compiled outputs from lifecycle
observers or access another unit's output without declaring a dependency.

## Notifications

Subscribe to `Deployer::before_deploy()` to prepare source files/configs
before the schema configuration compilation. It receives the entire
selected schema ID list. `after_deploy()` receives a DeploymentResult
after all workers have stopped and compiler cleanup has finished. It
contains the selected, successful and failed schema IDs and overall
success. Both notifications are synchronous and paired for every
started schema deployment batch, even if preparation or compilation
fails. Selection (including default configuration preparation) is guarded
against reentry. Invalid selection before a batch begins emits neither
notification.

Disconnect plugin connections during module finalization. Do not
reenter deployment or retain references to the event data. A throwing
before observer fails the batch; a throwing after observer is logged
without changing the compilation result. Other observers still run.
These notifications do not replace the frontend's deploy messages.

## Frontend deployment result and warmup

After joining maintenance, use `get_deployment_result` and release the returned snapshot with `free_deployment_result`. A missing/incomplete schema deployment has no snapshot. Compilation status is not runtime readiness. A frontend may report a partial failure when the initial schema is explicitly successful and other schemas explicitly failed, without other maintenance task failures (`deployment_has_other_failure`), but must not infer success from binary filenames.

`get_initial_schema` follows Switcher's selection rules without creating a session. On an idle engine owner thread, `prewarm_schema(nullptr, &paths)` prepares immutable resources via the runtime factories and returns their actual mapped absolute paths for optional bounded, cancellable background page reads. Release the list with `free_string_list`. Warmup neither deploys nor opens UserDB or creates a session; it is not a runtime integrity check. Pins for a single prepared schema survive editor/session changes and are released by explicit `release_prewarm`, before schema deployment or finalize. Serialize warmup with normal session operations; no duplicate preparation is done while a session exists.
