// Copyright (c) Facebook, Inc. and its affiliates.

// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
#include <chrono>
#include <iostream>
#include <numeric>
#include <thread>

#include <sys/ipc.h>
#include <sys/shm.h>

#include "yaml-cpp/yaml.h"

#include "real_time.hpp"

#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include "polymetis.grpc.pb.h"
#include "polymetis/utils.h"

#include <fstream>
#include <sstream>

#include <torch/jit.h>
#include <torch/script.h>

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;
using grpc::Status;

using namespace std::chrono_literals;

class TestGrpcClient {
public:
  TestGrpcClient(int num_dofs, std::shared_ptr<grpc::ChannelInterface> channel,
                 int num_requests, std::string robot_client_metadata_path)
      : stub_(PolymetisControllerServer::NewStub(channel)) {
    int log_iters = 3000;
    std::vector<float> times_taken;
    times_taken.reserve(log_iters);

    float global_max = -99999.0;
    float global_min = 99999.0;
    float global_avg = 0.0;

    // Load robot client metadata
    std::ifstream file(robot_client_metadata_path);
    assert(file);
    std::stringstream buffer;
    buffer << file.rdbuf();
    file.close();
    RobotClientMetadata metadata;
    assert(metadata.ParseFromString(buffer.str()));

    // Initialize robot client with metadata
    ClientContext context;
    Empty empty;
    Status status = stub_->InitRobotClient(&context, metadata, &empty);
    assert(status.ok());

    // Load shared memory
    segment_ = boost::interprocess::managed_shared_memory(
        boost::interprocess::open_only, "RobotStateSharedMemory");

    for (int i = 0; i < num_requests; i++) {
      auto start = std::chrono::high_resolution_clock::now();

      assert(SendCommand(num_dofs));

      auto end = std::chrono::high_resolution_clock::now();

      float ms_taken =
          ((float)std::chrono::duration_cast<std::chrono::nanoseconds>(end -
                                                                       start)
               .count()) /
          1000000.0;
      if (i % log_iters >= times_taken.size()) {
        times_taken.push_back(ms_taken);
      } else {
        times_taken.at(i % log_iters) = ms_taken;
      }
      if (ms_taken > 1.0) {
        std::cout << "\n==== Warning: round trip time takes " << ms_taken
                  << " ms! ====\n\n";
      }

      if (i > 0 && i % log_iters == 0) {
        float max_time_taken =
            *std::max_element(times_taken.begin(), times_taken.end());
        float min_time_taken =
            *std::min_element(times_taken.begin(), times_taken.end());

        float sum = 0.0;
        for (int j = 0; j < log_iters; j++) {
          sum += times_taken[j];
        }
        float avg_time_taken = sum / log_iters;

        std::cout << "max: " << max_time_taken << ", min: " << min_time_taken
                  << ", avg: " << avg_time_taken << std::endl;

        if (max_time_taken > global_max) {
          global_max = max_time_taken;
        }
        if (min_time_taken < global_min) {
          global_min = min_time_taken;
        }
        int num_logs_so_far = i / log_iters;
        global_avg = ((num_logs_so_far - 1) * global_avg + avg_time_taken) /
                     (float)num_logs_so_far;
        std::cout << "global max: " << global_max << ", min: " << global_min
                  << ", avg: " << global_avg << std::endl;
      }

      std::this_thread::sleep_until(start + 1ms);
    }
  }

private:
  std::unique_ptr<PolymetisControllerServer::Stub> stub_;
  boost::interprocess::managed_shared_memory segment_;
  bool SendCommand(int num_dofs) {
    RobotState dummy_state;

    auto timestamp = *segment_.find<ShmTimestamp>("shm_timestamp").first;

    auto joint_positions =
        *segment_.find<ShmVectorFloat>("joint_positions").first;
    auto joint_velocities =
        *segment_.find<ShmVectorFloat>("joint_velocities").first;
    auto joint_torques_measured =
        *segment_.find<ShmVectorFloat>("joint_torques_measured").first;
    auto joint_torques_external =
        *segment_.find<ShmVectorFloat>("joint_torques_external").first;

    setTimestampToNow(&timestamp);
    for (int i = 0; i < num_dofs; i++) {
      joint_positions[i] = 0.0;
      joint_velocities[i] = 0.0;
      joint_torques_measured[i] = 0.0;
      joint_torques_external[i] = 0.0;
    }

    TorqueCommand torque_command;
    ClientContext context;
    Status status =
        stub_->ControlUpdate(&context, dummy_state, &torque_command);
    if (!status.ok()) {
      std::cout << "SendCommand failed." << std::endl;
      return false;
    }
    return true;
  }
};

void *RunClient(void *cfg_ptr) {
  YAML::Node &config = *(static_cast<YAML::Node *>(cfg_ptr));

  int num_dofs = config["num_dofs"].as<int>();
  int num_requests = config["num_requests"].as<int>();
  std::string server_address = config["server_address"].as<std::string>();
  std::string robot_client_metadata_path =
      config["robot_client_metadata_path"].as<std::string>();

  TestGrpcClient client(
      num_dofs,
      grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials()),
      num_requests, robot_client_metadata_path);

  return NULL;
}

int main(int argc, char *argv[]) {
  if (argc != 2) {
    std::cout << "Usage: empty_statistics_client /path/to/cfg.yaml"
              << std::endl;
    return 1;
  }
  YAML::Node config = YAML::LoadFile(argv[1]);

  bool use_real_time = config["use_real_time"].as<bool>();
  void *config_void_ptr = static_cast<void *>(&config);

  if (!use_real_time) {
    RunClient(config_void_ptr);
  } else {
    create_real_time_thread(RunClient, config_void_ptr);
  }

  return 0;
}
