/*
 * Copyright 2017 The Cartographer Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "cartographer_grpc/mapping/trajectory_builder_stub.h"

#include "cartographer_grpc/proto/map_builder_service.pb.h"
#include "glog/logging.h"

namespace cartographer_grpc {
namespace mapping {

TrajectoryBuilderStub::TrajectoryBuilderStub(
    std::shared_ptr<grpc::Channel> client_channel, const int trajectory_id,
    LocalSlamResultCallback local_slam_result_callback)
    : client_channel_(client_channel), trajectory_id_(trajectory_id) {
  stub_ = proto::MapBuilderService::NewStub(client_channel_);
  CHECK(stub_) << "Failed to create stub.";
  if (local_slam_result_callback) {
    proto::ReceiveLocalSlamResultsRequest request;
    request.set_trajectory_id(trajectory_id);
    local_slam_result_reader_.client_reader = stub_->ReceiveLocalSlamResults(
        &local_slam_result_reader_.client_context, request);
    auto* client_reader_ptr = local_slam_result_reader_.client_reader.get();
    local_slam_result_reader_.thread =
        cartographer::common::make_unique<std::thread>(
            [client_reader_ptr, local_slam_result_callback]() {
              RunLocalSlamResultReader(client_reader_ptr,
                                       local_slam_result_callback);
            });
  }
}

TrajectoryBuilderStub::~TrajectoryBuilderStub() {
  if (local_slam_result_reader_.thread) {
    local_slam_result_reader_.thread->join();
  }
  if (rangefinder_writer_.client_writer) {
    CHECK(rangefinder_writer_.client_writer->WritesDone());
    CHECK(rangefinder_writer_.client_writer->Finish().ok());
  }
  if (imu_writer_.client_writer) {
    CHECK(imu_writer_.client_writer->WritesDone());
    CHECK(imu_writer_.client_writer->Finish().ok());
  }
  if (odometry_writer_.client_writer) {
    CHECK(odometry_writer_.client_writer->WritesDone());
    CHECK(odometry_writer_.client_writer->Finish().ok());
  }
  if (fixed_frame_writer_.client_writer) {
    CHECK(fixed_frame_writer_.client_writer->WritesDone());
    CHECK(fixed_frame_writer_.client_writer->Finish().ok());
  }
}

void TrajectoryBuilderStub::AddSensorData(
    const std::string& sensor_id,
    const cartographer::sensor::TimedPointCloudData& timed_point_cloud_data) {
  if (!rangefinder_writer_.client_writer) {
    rangefinder_writer_.client_writer = stub_->AddRangefinderData(
        &rangefinder_writer_.client_context, &rangefinder_writer_.response);
    CHECK(rangefinder_writer_.client_writer);
  }
  proto::AddRangefinderDataRequest request;
  *request.mutable_sensor_metadata() = CreateSensorMetadata(sensor_id);
  *request.mutable_timed_point_cloud_data() =
      cartographer::sensor::ToProto(timed_point_cloud_data);
  rangefinder_writer_.client_writer->Write(request);
}

void TrajectoryBuilderStub::AddSensorData(
    const std::string& sensor_id,
    const cartographer::sensor::ImuData& imu_data) {
  if (!imu_writer_.client_writer) {
    imu_writer_.client_writer =
        stub_->AddImuData(&imu_writer_.client_context, &imu_writer_.response);
    CHECK(imu_writer_.client_writer);
  }
  proto::AddImuDataRequest request;
  *request.mutable_sensor_metadata() = CreateSensorMetadata(sensor_id);
  *request.mutable_imu_data() = cartographer::sensor::ToProto(imu_data);
  imu_writer_.client_writer->Write(request);
}

void TrajectoryBuilderStub::AddSensorData(
    const std::string& sensor_id,
    const cartographer::sensor::OdometryData& odometry_data) {
  if (!odometry_writer_.client_writer) {
    odometry_writer_.client_writer = stub_->AddOdometryData(
        &odometry_writer_.client_context, &odometry_writer_.response);
    CHECK(odometry_writer_.client_writer);
  }
  proto::AddOdometryDataRequest request;
  *request.mutable_sensor_metadata() = CreateSensorMetadata(sensor_id);
  *request.mutable_odometry_data() =
      cartographer::sensor::ToProto(odometry_data);
  odometry_writer_.client_writer->Write(request);
}

void TrajectoryBuilderStub::AddSensorData(
    const std::string& sensor_id,
    const cartographer::sensor::FixedFramePoseData& fixed_frame_pose) {
  if (!fixed_frame_writer_.client_writer) {
    fixed_frame_writer_.client_writer = stub_->AddFixedFramePoseData(
        &fixed_frame_writer_.client_context, &fixed_frame_writer_.response);
    CHECK(fixed_frame_writer_.client_writer);
  }
  proto::AddFixedFramePoseDataRequest request;
  *request.mutable_sensor_metadata() = CreateSensorMetadata(sensor_id);
  *request.mutable_fixed_frame_pose_data() =
      cartographer::sensor::ToProto(fixed_frame_pose);
  fixed_frame_writer_.client_writer->Write(request);
}

proto::SensorMetadata TrajectoryBuilderStub::CreateSensorMetadata(
    const std::string& sensor_id) {
  proto::SensorMetadata sensor_metadata;
  sensor_metadata.set_sensor_id(sensor_id);
  sensor_metadata.set_trajectory_id(trajectory_id_);
  return sensor_metadata;
}

void TrajectoryBuilderStub::RunLocalSlamResultReader(
    grpc::ClientReader<proto::ReceiveLocalSlamResultsResponse>* client_reader,
    LocalSlamResultCallback local_slam_result_callback) {
  proto::ReceiveLocalSlamResultsResponse response;
  while (client_reader->Read(&response)) {
    int trajectory_id = response.trajectory_id();
    cartographer::common::Time time =
        cartographer::common::FromUniversal(response.timestamp());
    cartographer::transform::Rigid3d local_pose =
        cartographer::transform::ToRigid3(response.local_pose());
    cartographer::sensor::RangeData range_data =
        cartographer::sensor::FromProto(response.range_data());
    auto node_id =
        response.has_node_id()
            ? cartographer::common::make_unique<cartographer::mapping::NodeId>(
                  cartographer::mapping::NodeId{
                      response.node_id().trajectory_id(),
                      response.node_id().node_index()})
            : nullptr;
    local_slam_result_callback(trajectory_id, time, local_pose, range_data,
                               std::move(node_id));
  }
  client_reader->Finish();
}

}  // namespace mapping
}  // namespace cartographer_grpc
