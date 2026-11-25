//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-12-01 GONG Chen <chen.sst@gmail.com>
//
#ifndef RIME_DEPLOYER_H_
#define RIME_DEPLOYER_H_

#include <atomic>
#include <exception>
#include <future>
#include <mutex>
#include <queue>
#include <any>
#include <rime/common.h>
#include <rime/component.h>
#include <rime/messenger.h>

namespace rime {

class Deployer;

using TaskInitializer = std::any;

class DeploymentTask : public Class<DeploymentTask, TaskInitializer> {
 public:
  DeploymentTask() = default;
  virtual ~DeploymentTask() = default;

  virtual bool Run(Deployer* deployer) = 0;
};

// Call every deployment observer, even if another plugin throws.
struct DeploymentNotifierCombiner {
  using result_type = bool;

  template <class Iterator>
  bool operator()(Iterator first, Iterator last) const {
    bool success = true;
    for (; first != last; ++first) {
      try {
        (void)*first;
      } catch (const std::exception& ex) {
        LOG(ERROR) << "deployment observer failed: " << ex.what();
        success = false;
      } catch (...) {
        LOG(ERROR) << "deployment observer failed.";
        success = false;
      }
    }
    return success;
  }
};

struct DeploymentResult {
  vector<string> schema_ids;
  set<string> successful_schemas;
  set<string> failed_schemas;
  bool success = false;
};

class Deployer : public Messenger {
 public:
  using BeforeDeployCallback = void(Deployer*, const vector<string>&);
  using AfterDeployCallback = void(Deployer*, const DeploymentResult&);
  using BeforeDeployNotifier =
      signal<BeforeDeployCallback, DeploymentNotifierCombiner>;
  using AfterDeployNotifier =
      signal<AfterDeployCallback, DeploymentNotifierCombiner>;

  // Synchronous, once per schema deployment batch, including a single schema.
  // Before runs prior to compiling any schema configuration. After runs only
  // when all compile workers and compiler cleanup have finished, with results.
  // Observers must not reenter deployment or retain references to event data.
  BeforeDeployNotifier& before_deploy() { return before_deploy_; }
  AfterDeployNotifier& after_deploy() { return after_deploy_; }

  RIME_DLL bool RunSchemaDeployment(
      const function<bool(vector<string>*)>& select,
      const function<bool(DeploymentResult*)>& work);

  // read-only access after library initialization {
  path shared_data_dir;
  path user_data_dir;
  path prebuilt_data_dir;
  path staging_dir;
  path sync_dir;
  string user_id;
  string distribution_name;
  string distribution_code_name;
  string distribution_version;
  string app_name;
  bool backup_config_files;
  // }

  RIME_DLL Deployer();
  RIME_DLL ~Deployer();

  RIME_DLL bool RunTask(const string& task_name,
                        TaskInitializer arg = TaskInitializer());
  bool ScheduleTask(const string& task_name,
                    TaskInitializer arg = TaskInitializer());
  void ScheduleTask(an<DeploymentTask> task);
  an<DeploymentTask> NextTask();
  bool HasPendingTasks();

  bool Run();
  bool StartWork(bool maintenance_mode = false);
  bool StartMaintenance();
  bool IsWorking();
  bool IsMaintenanceMode();
  // the following two methods equally wait until all threads are joined
  void JoinWorkThread();
  void JoinMaintenanceThread();

  path user_data_sync_dir() const;

 private:
  BeforeDeployNotifier before_deploy_;
  AfterDeployNotifier after_deploy_;
  std::atomic<bool> deploying_schemas_{false};
  std::queue<of<DeploymentTask>> pending_tasks_;
  std::mutex mutex_;
  std::future<void> work_;
  bool maintenance_mode_ = false;
};

}  // namespace rime

#endif  // RIME_DEPLOYER_H_
