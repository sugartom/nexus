#ifndef NEXUS_SCHEDULER_SCHEDULER_H_
#define NEXUS_SCHEDULER_SCHEDULER_H_

#include <chrono>
#include <deque>
#include <grpc++/grpc++.h>
#include <gtest/gtest.h>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <yaml-cpp/yaml.h>

#include "nexus/common/rpc_call.h"
#include "nexus/common/rpc_service_base.h"
#include "nexus/proto/control.grpc.pb.h"
#include "nexus/scheduler/backend_delegate.h"
#include "nexus/scheduler/frontend_delegate.h"

namespace nexus {
namespace scheduler {

using AsyncService = nexus::SchedulerCtrl::AsyncService;
using BackendDelegatePtr = std::shared_ptr<BackendDelegate>;
using FrontendDelegatePtr = std::shared_ptr<FrontendDelegate>;
using ServerList = std::unordered_set<uint32_t>;

struct ModelInfo {
  std::unordered_map<uint32_t, double> backend_throughputs;
  std::unordered_set<uint32_t> subscribers;
  std::deque<double> rps_history;

  double total_throughput() const {
    double total = 0.;
    for (auto iter : backend_throughputs) {
      total += iter.second;
    }
    return total;
  }
};

/*! \brief Scheduler acts as a global centralized scheduler server. */
class Scheduler : public AsyncRpcServiceBase<AsyncService> {
 public:
  /*!
   * \brief Constructor for Scheduler object.
   * \param address IP address and port, e.g., 127.0.0.1:1234.
   * \param nthreads Number of threads that handle the RPC calls.
   * \param beacon_interval Beacon interval in seconds.
   * \param epoch_interval Epoch interval in seconds.
   */
  Scheduler(std::string port, size_t nthreads, std::string db_root_dir,
            uint32_t beacon_interval, uint32_t epoch_interval);
  /*!
   * \brief Loads the workload configuation for backends from config file.
   * \param config_file Config file path.
   */
  void LoadWorkloadFile(const std::string& workload_file);
  /*!
   * \brief Starts the scheduler main thread that monitors the server
   *    aliveness and changes in workload. */
  void Run();
  /*!
   * \brief Handles Register RPC.
   *
   *   This function doesn't acquire mutex_.
   * \param ctx RPC server context
   * \param request Register request
   * \param reply Reply to register RPC
   */
  void Register(const grpc::ServerContext& ctx, const RegisterRequest& request,
                RegisterReply* reply);
  /*!
   * \brief Handles Unregister RPC.
   *
   *   This function doesn't acquire mutex_.
   * \param ctx RPC server context
   * \param request Unregister request
   * \param reply Reply to unregister RPC
   */
  void Unregister(const grpc::ServerContext& ctx,
                  const UnregisterRequest& request, RpcReply* reply);
  /*!
   * \brief Handles LoadModel RPC.
   *
   *   This function acquires mutex_.
   * \param ctx RPC server context
   * \param request Load model request
   * \param reply Reply to RPC
   */
  void LoadModel(const grpc::ServerContext& ctx,
                 const LoadModelRequest& request, LoadModelReply* reply);
  /*!
   * \brief Handles UpdateBackendStats RPC.
   *
   *   This function acquires mutex_.
   * \param ctx RPC server context
   * \param request Backend stats information
   * \param reply Reply to RPC
   */
  void UpdateBackendStats(const grpc::ServerContext& ctx,
                          const BackendStatsProto& request, RpcReply* reply);
  /*!
   * \brief Handles KeepAlive RPC.
   *
   *   This function acquires mutex_.
   * \param ctx RPC server context
   * \param request Keep alive request
   * \param reply Reply to RPC
   */
  void KeepAlive(const grpc::ServerContext& ctx,
                 const KeepAliveRequest& request, RpcReply* reply);

 private:
  /*! \brief Initializes RPC handlers. */
  void HandleRpcs() final;
  /*!
   * \brief Registers frontend RPC client and fills in the register reply.
   *
   *    This function acquires mutex_.
   * \param frontend Frontend RPC client pointer.
   * \param reply Register reply.
   */
  void RegisterFrontend(FrontendDelegatePtr frontend, RegisterReply* reply);
  /*!
   * \brief Registers backend RPC client and fills in the register reply.
   *
   *    This function acquires mutex_.
   * \param backend Backend RPC client pointer.
   * \param reply Register reply.
   */
  void RegisterBackend(BackendDelegatePtr backend, RegisterReply* reply);
  /*!
   * \brief Unregister frontend RPC client and fills in the register reply
   *
   *    This function acquires mutex_.
   * \param node_id Frontend node ID.
   */
  void UnregisterFrontend(uint32_t node_id);
  /*!
   * \brief Unregister frontend RPC client and fills in the register reply
   *
   *    This function acquires mutex_.
   * \param node_id Backend node ID.
   */
  void UnregisterBackend(uint32_t node_id);
  /*!
   * \brief Update workload to the new added backend
   *
   *   This function doesn't acquire mutex_.
   * \param backend Backend client pointer
   */
  void AddBackend(BackendDelegatePtr backend);
  /*!
   * \brief Assign the workload of the removed backend to other idle ones.
   *
   *   This function doesn't acquire mutex_.
   * \param backend Backend client pointer
   */
  void RemoveBackend(BackendDelegatePtr backend);
  /*!
   * \brief Update the model subscribers, and potentially remove the model
   *   sessions if no one subscribes it.
   *
   *   This function doesn't acquire mutex_.
   * \param backend Backend client pointer
   */
  void RemoveFrontend(FrontendDelegatePtr frontend);
  /*!
   * \brief Get backend rpc client given the node id.
   *
   *   This function doesn't acquire mutex_.
   * \param node_id Backend node id.
   * \return BackendDelegate pointer if found, otherwise nullptr
   */
  BackendDelegatePtr GetBackend(uint32_t node_id);
  /*!
   * \brief Get frontend rpc client given the node id.
   *
   *   This function doesn't acquire mutex_.
   * \param node_id Frontend node id.
   * \return FrontendDelegate pointer if found, otherwise nullptr
   */
  FrontendDelegatePtr GetFrontend(uint32_t node_id);
  /*!
   * \brief Get the model route given the model session id.
   *
   *   This function doesn't acquire mutex_.
   * \param model_session_id Model session ID.
   * \param route Model route to fill in.
   */
  void GetModelRoute(const std::string& model_session_id,
                     ModelRouteProto* route);
  /*!
   * \brief Find the best-fit backend to load the model session with workload.
   *
   *   This function doesn't acquire mutex_.
   * \param model_sess Model session.
   * \param request_rate Requests per second.
   * \param skips Backends that should be skipped.
   * \param best_backend Best-fit backend pointer.
   * \param inst_cfg Model instance configuration to be loaded.
   */
  void FindBestBackend(const ModelSession& model_sess, float request_rate,
                       const std::unordered_set<uint32_t>& skips,
                       BackendDelegatePtr* best_backend,
                       ModelInstanceConfig* inst_cfg);
  /*!
   * \brief At each beacon cycle, check whether frontends and backends are
   *   alive, and aggregate model session request rates from backends.
   *
   *   This function acquires mutex_.
   */
  void BeaconCheck();
  /*!
   * \brief At each epoch cycle, re-schedule the resources for all model
   *   sessions based on the request rates during last epoch
   *
   *   This function acquires mutex_.
   */
  void EpochSchedule();

  void AllocateUnassignedWorkloads(
      std::unordered_set<std::string>* changed_routes,
      std::unordered_set<BackendDelegatePtr>* changed_backends = nullptr);

  void UpdateModelRoutes(std::unordered_set<std::string> model_sessions);

  void DisplayModelTable();

  friend class SchedulerTest;
  FRIEND_TEST(SchedulerTest, EpochSchedule);

  /*! \brief Beacon interval in seconds */
  uint32_t beacon_interval_sec_;
  /*! \brief Epoch duration in seconds */
  uint32_t epoch_interval_sec_;
  uint32_t history_len_;
  /*! \brief Static workload configuration */
  std::vector<std::vector<YAML::Node> > static_workloads_;
  /*! \brief Mapping from static workload id to backend node id */
  std::unordered_map<int, uint32_t> assigned_static_workloads_;
  /*! \brief Unassigned workloads in <model session id, request rate> pairs */
  std::vector<std::pair<std::string, float> > unassigned_workloads_;
  /*! \brief Mapping from frontend node id to frontend client */
  std::unordered_map<uint32_t, FrontendDelegatePtr> frontends_;
  /*! \brief Mapping from backend node id to backend client */
  std::unordered_map<uint32_t, BackendDelegatePtr> backends_;
  /*! \brief Mapping from model session ID to model information */
  std::unordered_map<std::string, ModelInfo> model_table_;
  /*! \brief Mutex for accessing internal data */
  std::mutex mutex_;
};

} // namespace scheduler
} // namespace nexus


#endif // NEXUS_SCHEDULER_SCHEDULER_H_
