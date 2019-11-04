/*
 * Copyright (C) 2012-2013 Simon Lynen, ASL, ETH Zurich, Switzerland
 * You can contact the author at <slynen at ethz dot ch>
 * Copyright (C) 2011-2012 Stephan Weiss, ASL, ETH Zurich, Switzerland
 * You can contact the author at <stephan dot weiss at ieee dot org>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef POSITION_ATTITUDE_VELOCITY_MEASUREMENTMANAGER_H
#define POSITION_ATTITUDE_VELOCITY_MEASUREMENTMANAGER_H

#include <ros/ros.h>

#include <msf_core/msf_core.h>
#include <msf_core/msf_sensormanagerROS.h>
#include <msf_core/msf_IMUHandler_ROS.h>
#include "msf_statedef.hpp"
#include <msf_updates/position_sensor_handler/position_sensorhandler.h>
#include <msf_updates/position_sensor_handler/position_measurement.h>
#include <msf_updates/attitude_sensor_handler/attitude_sensorhandler.h>
#include <msf_updates/velocity_sensor_handler/velocity_sensorhandler.h>
#include <msf_updates/PositionVelocitySensorConfig.h>


namespace msf_position_velocity_sensor {

typedef msf_updates::PositionVelocitySensorConfig Config_T;
typedef dynamic_reconfigure::Server<Config_T> ReconfigureServer;
typedef shared_ptr<ReconfigureServer> ReconfigureServerPtr;

class PositionVelocitySensorManager : public msf_core::MSF_SensorManagerROS<
    msf_updates::EKFState> {
  typedef msf_position_sensor::PositionSensorHandler<
      msf_updates::position_measurement::PositionMeasurement,
      PositionVelocitySensorManager> PositionSensorHandler_T;
  friend class msf_position_sensor::PositionSensorHandler<
      msf_updates::position_measurement::PositionMeasurement,
      PositionVelocitySensorManager> ;
 public:
  typedef msf_updates::EKFState EKFState_T;
  typedef EKFState_T::StateSequence_T StateSequence_T;
  typedef EKFState_T::StateDefinition_T StateDefinition_T;

  PositionVelocitySensorManager(
      ros::NodeHandle pnh = ros::NodeHandle("~/position_velocity_sensor")) {
    imu_handler_.reset(
        new msf_core::IMUHandler_ROS<msf_updates::EKFState>(*this, "msf_core",
                                                            "imu_handler"));

    position_handler_.reset(
        new PositionSensorHandler_T(*this, "", "position_velocity_sensor"));
    AddHandler(position_handler_);

    velocity_handler_.reset(
        new msf_velocity_sensor::VelocitySensorHandler(*this, "",
                                                       "velocity_sensor"));
    AddHandler(velocity_handler_);

    attitude_handler_.reset(
                new msf_attitude_sensor::AttitudeSensorHandler(*this, "",
                                                               "attitude_sensor"));
    AddHandler(attitude_handler_);

    reconf_server_.reset(new ReconfigureServer(pnh));
    ReconfigureServer::CallbackType f = boost::bind(
        &PositionVelocitySensorManager::Config, this, _1, _2);
    reconf_server_->setCallback(f);
  }
  virtual ~PositionVelocitySensorManager() {
  }

  virtual const Config_T& Getcfg() {
    return config_;
  }

 private:
  shared_ptr<msf_core::IMUHandler_ROS<msf_updates::EKFState> > imu_handler_;
  shared_ptr<PositionSensorHandler_T> position_handler_;
  shared_ptr<msf_velocity_sensor::VelocitySensorHandler> velocity_handler_;
  shared_ptr<msf_attitude_sensor::AttitudeSensorHandler> attitude_handler_;


  Config_T config_;
  ReconfigureServerPtr reconf_server_;

  /**
   * \brief Dynamic reconfigure callback.
   */
  virtual void Config(Config_T &config, uint32_t level) {
    config_ = config;
    position_handler_->SetNoises(config.position_noise_meas);
    position_handler_->SetDelay(config.position_delay);
    velocity_handler_->SetNoises(config.press_noise_meas_v);
    attitude_handler_->SetNoises(1e-6);

    if ((level & msf_updates::PositionVelocitySensor_INIT_FILTER)
        && config.core_init_filter == true) {
      Init(1.0);
      config.core_init_filter = false;
    }
 }

  void Init(double scale) const {
    if (scale < 0.001) {
      MSF_WARN_STREAM("init scale is "<<scale<<" correcting to 1");
      scale = 1;
    }

    Eigen::Matrix<double, 3, 1> p, v, b_w, b_a, g, w_m, a_m, p_ip, p_vc;
    Eigen::Quaternion<double> q;
    msf_core::MSF_Core<EKFState_T>::ErrorStateCov P;

    // Init values.
    g << 0, 0, 9.81;  /// Gravity.
    b_w << 0, 0, 0;		/// Bias gyroscopes.
    b_a << 0, 0, 0;		/// Bias accelerometer.

    v << 0, 0, 0;		/// Robot velocity (IMU centered).
    w_m << 0, 0, 0;		/// Initial angular velocity.

    // Set the initial yaw alignment of body to world (the frame in which the
    // position sensor measures).
    double yawinit = config_.position_yaw_init / 180 * M_PI;
    Eigen::Quaterniond yawq(cos(yawinit / 2), 0, 0, sin(yawinit / 2));
    yawq.normalize();

    q = yawq;

    P.setZero();  // Error state covariance; if zero, a default initialization
                  // in msf_core is used

    p_vc = position_handler_->GetPositionMeasurement();

    MSF_INFO_STREAM(
        "initial measurement pos:[" << p_vc.transpose() << "] orientation: " << STREAMQUAT(q));

    // check if we have already input from the measurement sensor
    if (!position_handler_->ReceivedFirstMeasurement())
      MSF_WARN_STREAM(
          "No measurements received yet to initialize position - using [0 0 0]");

    ros::NodeHandle pnh("~");
    pnh.param("position_sensor/init/p_ip/x", p_ip[0], 0.0);
    pnh.param("position_sensor/init/p_ip/y", p_ip[1], 0.0);
    pnh.param("position_sensor/init/p_ip/z", p_ip[2], 0.0);

    // Calculate initial attitude and position based on sensor measurements.
    p = p_vc - q.toRotationMatrix() * p_ip;

    a_m = q.inverse() * g;			    /// Initial acceleration.

    //prepare init "measurement"
    // True means that we will also set the initialsensor readings.
    shared_ptr < msf_core::MSF_InitMeasurement<EKFState_T>
        > meas(new msf_core::MSF_InitMeasurement<EKFState_T>(true));

    meas->SetStateInitValue < StateDefinition_T::p > (p);
    meas->SetStateInitValue < StateDefinition_T::v > (v);
    meas->SetStateInitValue < StateDefinition_T::q > (q);
    meas->SetStateInitValue < StateDefinition_T::b_w > (b_w);
    meas->SetStateInitValue < StateDefinition_T::b_a > (b_a);
    meas->SetStateInitValue < StateDefinition_T::p_ip > (p_ip);

    SetStateCovariance(meas->GetStateCovariance());  // Call my set P function.
    meas->Getw_m() = w_m;
    meas->Geta_m() = a_m;
    meas->time = ros::Time::now().toSec();

    // Call initialization in core.
    msf_core_->Init(meas);
  }

  // Prior to this call, all states are initialized to zero/identity.
  virtual void ResetState(EKFState_T& state) const {
    UNUSED(state);
  }
  virtual void InitState(EKFState_T& state) const {
    UNUSED(state);
  }

  virtual void CalculateQAuxiliaryStates(EKFState_T& state, double dt) const {
    const msf_core::Vector3 npipv = msf_core::Vector3::Constant(
        config_.position_noise_p_ip);

    // Compute the blockwise Q values and store them with the states,
    //these then get copied by the core to the correct places in Qd.
    state.GetQBlock<StateDefinition_T::p_ip>() =
        (dt * npipv.cwiseProduct(npipv)).asDiagonal();
  }

  virtual void SetStateCovariance(
      Eigen::Matrix<double, EKFState_T::nErrorStatesAtCompileTime,
          EKFState_T::nErrorStatesAtCompileTime>& P) const {
    UNUSED(P);
    // Nothing, we only use the simulated cov for the core plus diagonal for the
    // rest.
  }

  virtual void AugmentCorrectionVector(
      Eigen::Matrix<double, EKFState_T::nErrorStatesAtCompileTime, 1>& correction) const {
    UNUSED(correction);
  }

  virtual void SanityCheckCorrection(
      EKFState_T& delaystate,
      const EKFState_T& buffstate,
      Eigen::Matrix<double, EKFState_T::nErrorStatesAtCompileTime, 1>& correction) const {
    UNUSED(delaystate);
    UNUSED(buffstate);
    UNUSED(correction);
  }
};
}
#endif  // POSITION_ATTITUDE_VELOCITY_MEASUREMENTMANAGER_H
