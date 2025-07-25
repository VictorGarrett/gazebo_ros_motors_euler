#include <algorithm>
#include <assert.h>
#include <ignition/math/Angle.hh>
#include <ignition/math/Pose3.hh>
#include <ignition/math/Quaternion.hh>
#include <ignition/math/Vector3.hh>
#include <sdf/sdf.hh>
#include <ros/ros.h>

#include "gazebo_ros_dc_motor/gazebo_ros_dc_motor.h"

namespace gazebo {

// Constructor
GazeboRosMotor::GazeboRosMotor() {
}

// Destructor
GazeboRosMotor::~GazeboRosMotor() {
  FiniChild();
}

// Load the controller
void GazeboRosMotor::Load ( physics::ModelPtr _parent, sdf::ElementPtr _sdf ) {
    this->parent = _parent;
    this->plugin_name_ = _sdf->GetAttribute("name")->GetAsString();

    gazebo_ros_ = GazeboRosPtr ( new GazeboRos ( _parent, _sdf, plugin_name_ ) );
    gazebo_ros_->isInitialized();

    // start custom queue
    this->callback_queue_thread_ = std::thread ( std::bind ( &GazeboRosMotor::QueueThread, this ) );

    // global parameters
    gazebo_ros_->getParameter<std::string> ( command_topic_,  "command_topic",  "/motor/voltage_norm" );
    gazebo_ros_->getParameter<double> ( update_rate_, "update_rate", 100.0 );

    // motor model parameters
    gazebo_ros_->getParameter<double> ( motor_nominal_voltage_, "motor_nominal_voltage", 24.0 ); // Datasheet 24.0V
    ROS_INFO_NAMED(plugin_name_, "motor_nominal_voltage_ = %f", motor_nominal_voltage_);
    gazebo_ros_->getParameter<double> ( moment_of_inertia_, "moment_of_inertia", 0.001 ); // 0.001 kgm^2
    ROS_INFO_NAMED(plugin_name_, "moment_of_inertia_ = %f", moment_of_inertia_);
    gazebo_ros_->getParameter<double> ( armature_damping_ratio_, "armature_damping_ratio", 0.0001 ); // Nm/(rad/s)
    ROS_INFO_NAMED(plugin_name_, "armature_damping_ratio_ = %f", armature_damping_ratio_);
    gazebo_ros_->getParameter<double> ( electromotive_force_constant_, "electromotive_force_constant", 0.08 ); // Datasheet: 1.8Nm / 22A or 24V / 300 (rad/s)
    ROS_INFO_NAMED(plugin_name_, "electromotive_force_constant_ = %f", electromotive_force_constant_);
    gazebo_ros_->getParameter<double> ( electric_resistance_, "electric_resistance", 1.0 ); // 1 Ohm
    ROS_INFO_NAMED(plugin_name_, "electric_resistance_ = %f", electric_resistance_);
    gazebo_ros_->getParameter<double> ( electric_inductance_, "electric_inductance", 0.001 ); // 1 mH
    ROS_INFO_NAMED(plugin_name_, "electric_inductance_ = %f", electric_inductance_);

    // noise parameters
    gazebo_ros_->getParameter<double> ( velocity_noise_, "velocity_noise", 0.0 );

    // gearbox parameters
    gazebo_ros_->getParameter<double> ( gear_ratio_, "gear_ratio", 1.0 ); // Reduction!
    ROS_INFO_NAMED(plugin_name_, "gear_ratio_ = %f", gear_ratio_);

    // encoder parameters
    gazebo_ros_->getParameterBoolean  ( publish_velocity_, "publish_velocity", true );
    gazebo_ros_->getParameterBoolean  ( publish_encoder_, "publish_encoder", false );
    gazebo_ros_->getParameterBoolean  ( publish_current_, "publish_current", true );
    gazebo_ros_->getParameter<int>    ( encoder_pulses_per_revolution_, "encoder_ppr", 4096 );

    gazebo_ros_->getParameter<std::string> ( velocity_topic_, "velocity_topic", "/motor/velocity" );
    gazebo_ros_->getParameter<std::string> ( encoder_topic_,  "encoder_topic",  "/motor/encoder"  );
    gazebo_ros_->getParameter<std::string> ( current_topic_,  "current_topic",  "/motor/current"  );
    gazebo_ros_->getParameter<std::string> ( supply_topic_,  "supply_topic",  "/motor/supply_voltage"  );

    // motor joint
    joint_ = gazebo_ros_->getJoint ( parent, "motor_shaft_joint", "shaft_joint" );

    // shaft link
    gazebo_ros_->getParameter<std::string> ( wrench_frame_,  "motor_wrench_frame", "wheel_link" );
    this->link_ = parent->GetLink(this->wrench_frame_);
    if (!this->link_) {
      ROS_FATAL_NAMED(plugin_name_, "link named: %s does not exist\n",this->wrench_frame_.c_str());
      return;
    }

    // joint state publisher
    gazebo_ros_->getParameterBoolean  ( publish_motor_joint_state_, "publish_motor_joint_state", false );
    if (this->publish_motor_joint_state_) {
        joint_state_publisher_ = gazebo_ros_->node()->advertise<sensor_msgs::JointState>(joint_->GetName()+"/joint_state", 1000);
        ROS_INFO_NAMED(plugin_name_, "%s: Advertise joint_state", gazebo_ros_->info());
    }
    this->joint_->SetProvideFeedback(true);

    if ( this->update_rate_ > 0.0 ) this->update_period_ = 1.0 / this->update_rate_; else this->update_period_ = 0.0;
    last_update_time_ = parent->GetWorld()->SimTime();

    // command subscriber
    ROS_INFO_NAMED(plugin_name_, "%s: Trying to subscribe to %s", gazebo_ros_->info(), command_topic_.c_str());
    ros::SubscribeOptions so = ros::SubscribeOptions::create<std_msgs::Float32> (
        command_topic_,
        1,
        boost::bind(&GazeboRosMotor::cmdVelCallback, this, _1),
        ros::VoidPtr(),
        &queue_
    );
    cmd_vel_subscriber_ = gazebo_ros_->node()->subscribe(so);
    ROS_INFO_NAMED(plugin_name_, "%s: Subscribed to %s", gazebo_ros_->info(), command_topic_.c_str());

    // supply voltage subscriber
    ROS_INFO_NAMED(plugin_name_, "%s: Trying to subscribe to %s", gazebo_ros_->info(), supply_topic_.c_str());
    ros::SubscribeOptions sov = ros::SubscribeOptions::create<std_msgs::Float32> (
        supply_topic_,
        1,
        boost::bind(&GazeboRosMotor::supplyVoltageCallBack, this, _1),
        ros::VoidPtr(),
        &queue_
    );
    supply_voltage_subscriber_ = gazebo_ros_->node()->subscribe(sov);
    ROS_INFO_NAMED(plugin_name_, "%s: Subscribed to %s", gazebo_ros_->info(), command_topic_.c_str());

    // encoder publishers
    if (this->publish_velocity_){
      velocity_publisher_ = gazebo_ros_->node()->advertise<std_msgs::Float32>(velocity_topic_, 1);
      ROS_INFO_NAMED(plugin_name_, "%s: Advertising motor shaft (before gearbox) velocity on %s ", gazebo_ros_->info(), velocity_topic_.c_str());
    }
    if (this->publish_encoder_){
      encoder_publisher_ = gazebo_ros_->node()->advertise<std_msgs::Int32>(encoder_topic_, 1);
      ROS_INFO_NAMED(plugin_name_, "%s: Advertising encoder counts on %s ", gazebo_ros_->info(), encoder_topic_.c_str());
    }
    if (this->publish_current_){
      current_publisher_ = gazebo_ros_->node()->advertise<std_msgs::Float32>(current_topic_, 1);
      ROS_INFO_NAMED(plugin_name_, "%s: Advertising actual motor current on %s ", gazebo_ros_->info(), current_topic_.c_str());
    }

    // listen to the update event (broadcast every simulation iteration)
    this->update_connection_ = event::Events::ConnectWorldUpdateBegin ( boost::bind ( &GazeboRosMotor::UpdateChild, this ) );

    input_ = 0;
    encoder_counter_ = 0;
    internal_current_ = 0;
    internal_omega_ = 0;
    supply_voltage_ = motor_nominal_voltage_;

    // Set up dynamic_reconfigure server
    ROS_INFO_NAMED(plugin_name_, "%s: Setting up dynamic reconfigure server.", gazebo_ros_->info());
    node_handle_ = new ros::NodeHandle(plugin_name_);
    dynamic_reconfigure_server_.reset(new dynamic_reconfigure::Server<gazebo_ros_motors::motorModelConfig>(reconf_mutex_, ros::NodeHandle(*node_handle_)));
    boost::recursive_mutex::scoped_lock scoped_lock(reconf_mutex_);
    notify_server_ = true;
    this->paramServerUpdate();
    dynamic_reconfigure_server_->setCallback(boost::bind(&GazeboRosMotor::reconfigureCallBack, this, _1, _2));
    scoped_lock.unlock();
}

void GazeboRosMotor::Reset() {
  last_update_time_ = parent->GetWorld()->SimTime();
  input_ = 0;
  encoder_counter_ = 0;
  internal_current_ = 0;
  internal_omega_ = 0;
  supply_voltage_ = motor_nominal_voltage_;
}

bool GazeboRosMotor::checkParameters() {
   if ( this->armature_damping_ratio_ !=0.0 && !isnan(this->armature_damping_ratio_) &&
        this->electric_inductance_ !=0.0 && !isnan(this->electric_inductance_) &&
        this->electric_resistance_ !=0.0 && !isnan(this->electric_resistance_) &&
        this->electromotive_force_constant_ !=0.0 && !isnan(this->electromotive_force_constant_) &&
        this->moment_of_inertia_ !=0.0 && !isnan(this->moment_of_inertia_)) return true;
        else return false;
}

bool GazeboRosMotor::ValidateParameters() {
    const double& d = this->armature_damping_ratio_;
    const double& L = this->electric_inductance_;
    const double& R = this->electric_resistance_;
    const double& Km = this->electromotive_force_constant_;
    const double& J = this->moment_of_inertia_;
    bool ok = true;
    // Check if d^2 L^2 + J^2 R^2 - 2 J L (2 Km^2 + d R) > 0 (which appears under sqrt)
    double Om = 0;
    if (d*d*L*L + J*J*R*R > 2*J*L*(2*Km*Km + d*R)) {
        Om = sqrt(d*d*L*L + J*J*R*R - 2*J*L*(2*Km*Km + d*R));
        // OK, roots are real, not complex
    } else {
        ok = false;
        ROS_WARN_NAMED(plugin_name_, "Incorrect DC motor parameters: d^2 L^2 + J^2 R^2 - 2 J L (2 Km^2 + d R) > 0 not satisfied!");
    }

    if (ok) {
      // Check if -dL-JR+Om < 0 (Other real root is always negative: -dL-JR-Om)
      if (Om < d*L+J*R) {
        // OK, both real roots are stable
      } else {
        ok = false;
        ROS_WARN_NAMED(plugin_name_, "Incorrect DC motor parameters: sqrt(d^2 L^2 + J^2 R^2 - 2 J L (2 Km^2 + d R)) < d*L+J*R not satisfied!");
      }
    }
    return ok;
}

void GazeboRosMotor::paramServerUpdate()
{
  if (notify_server_) {
    current_config_.velocity_noise        = velocity_noise_;
    current_config_.motor_nominal_voltage = motor_nominal_voltage_;
    current_config_.electric_resistance   = electric_resistance_;
    current_config_.electric_inductance   = electric_inductance_;
    current_config_.moment_of_inertia     = moment_of_inertia_;
    current_config_.armature_damping_ratio       = armature_damping_ratio_;
    current_config_.electromotive_force_constant = electromotive_force_constant_;
    dynamic_reconfigure_server_->updateConfig(current_config_);
    ROS_INFO_NAMED(plugin_name_, "Notifying parameter server...");
    notify_server_ = false;
  }
}

void GazeboRosMotor::reconfigureCallBack(const gazebo_ros_motors::motorModelConfig &config, uint32_t level) {

  if (this->checkParameters())
  {
    // Noise and V does not affect linear stability
    this->velocity_noise_=config.velocity_noise;
    this->motor_nominal_voltage_=config.motor_nominal_voltage;
    bool ok = true;

    double temp = this->electric_resistance_; // Temporary storage for parameter
    // ROS_INFO_NAMED(plugin_name_, "Rt = %f", temp);
    if (electric_resistance_ != config.electric_resistance) {
      electric_resistance_ = config.electric_resistance;// R
      if (!this->ValidateParameters()) {
    	electric_resistance_ = temp;
    	ROS_WARN_NAMED(plugin_name_, "Electric resistance %6.3f discarded, keeping previous value of %6.3f Ohm", config.electric_resistance, electric_resistance_);
    	ok = false;
      }
    }

    temp = this->electric_inductance_;
    // ROS_INFO_NAMED(plugin_name_, "Lt = %f", temp);
    if (electric_inductance_ != config.electric_inductance) {
      electric_inductance_ = config.electric_inductance; // L
      if (!this->ValidateParameters()) {
    	electric_inductance_ = temp;
    	ROS_WARN_NAMED(plugin_name_, "Electric inductance %6.3f discarded, keeping previous value of %6.3f H", config.electric_inductance, electric_inductance_);
    	ok = false;
      }
    }

    temp = this->moment_of_inertia_;
    // ROS_INFO_NAMED(plugin_name_, "Jt = %f", temp);
    if (moment_of_inertia_ != config.moment_of_inertia) {
      moment_of_inertia_ = config.moment_of_inertia; // J
      if (!this->ValidateParameters()) {
    	moment_of_inertia_ = temp;
    	ROS_WARN_NAMED(plugin_name_, "Moment of inertia %6.3f discarded, keeping previous value of %6.3f kgm^2", config.moment_of_inertia, moment_of_inertia_);
    	ok = false;
      }
    }

    temp = this->armature_damping_ratio_;
    // ROS_INFO_NAMED(plugin_name_, "Dt = %f", temp);
    if (armature_damping_ratio_ != config.armature_damping_ratio) {
      armature_damping_ratio_ = config.armature_damping_ratio; // d
      if (!this->ValidateParameters()) {
    	armature_damping_ratio_ = temp;
    	ROS_WARN_NAMED(plugin_name_, "Armature damping %6.3f discarded, keeping previous value of %6.3f Nm/(rad/s)", config.armature_damping_ratio, armature_damping_ratio_);
    	ok = false;
      }
    }

    temp = this->electromotive_force_constant_;
    // ROS_INFO_NAMED(plugin_name_, "Kt = %f", temp);
    if (electromotive_force_constant_ != config.electromotive_force_constant) {
      electromotive_force_constant_ = config.electromotive_force_constant; // Km
      if (!this->ValidateParameters()) {
    	electromotive_force_constant_ = temp;
    	ROS_WARN_NAMED(plugin_name_, "Motor constant %6.3f discarded, keeping previous value of %6.3f Nm/(rad/s)", config.electromotive_force_constant, electromotive_force_constant_);
    	ok = false;
      }
    }

    if (ok) {
      ROS_INFO_NAMED(plugin_name_, "DC Motor parameters validated and updated");
    } else {
      notify_server_ = true; // Some of the params were rejected, notify the param server from the main loop.
    }
  }
}

void GazeboRosMotor::publishWheelJointState(double velocity, double effort) {
    if (this->publish_motor_joint_state_){
    ros::Time current_time = ros::Time::now();
    joint_state_.header.stamp = current_time;
    joint_state_.name.resize ( 1 );
    joint_state_.position.resize ( 1 );
    joint_state_.velocity.resize ( 1 );
    joint_state_.effort.resize ( 1 );
    physics::JointPtr joint = joint_;
    double position = joint->Position ( 0 );
    joint_state_.name[0] = joint->GetName();
    joint_state_.position[0] = position;
    joint_state_.velocity[0] = velocity;
    joint_state_.effort[0] = effort;
    joint_state_publisher_.publish ( joint_state_ );
    }
}

// Velocity publisher
void GazeboRosMotor::publishRotorVelocity(double m_vel){
  std_msgs::Float32 vel_msg;
  vel_msg.data = m_vel; // (rad/sec)
  if (this->publish_velocity_) velocity_publisher_.publish(vel_msg);
}

// Simple incremental encoder emulation
void GazeboRosMotor::publishEncoderCount(double m_vel, double dT){
  std_msgs::Int32 counter_msg;
  double rev_in_rad = m_vel * dT;
  encoder_counter_ += ((rev_in_rad)/(2*M_PI)) * encoder_pulses_per_revolution_;
  counter_msg.data = encoder_counter_;
  if (this->publish_encoder_) encoder_publisher_.publish(counter_msg);
}

void GazeboRosMotor::publishMotorCurrent(){
  std_msgs::Float32 c_msg;
  c_msg.data = internal_current_; // (amps)
  if (this->publish_current_) current_publisher_.publish(c_msg);
}


void GazeboRosMotor::motorModelUpdate(double dt, double output_shaft_omega, double total_torque) {
    input_ = std::clamp(input_, -1.0, 1.0);

    double V = input_ * supply_voltage_;


    double internal_omega = output_shaft_omega * gear_ratio_;
    double dw = internal_omega - internal_omega_;

    // Compute motor current using DC motor equation (no inductance)
    double i_t = (V - internal_omega * electromotive_force_constant_) / electric_resistance_;


    // Compute torque on motor shaft
    double internal_torque = electromotive_force_constant_ * i_t - internal_omega*armature_damping_ratio_;

    // Reflect torque to output shaft
    double output_torque = internal_torque * gear_ratio_;

    // Apply torque to wheel link in Z-axis (assumes wheel spins around Z)
    ignition::math::Vector3d applied_torque;
    applied_torque.Z() = output_torque;
    link_->AddRelativeTorque(applied_torque);

    internal_current_ = i_t;
    internal_omega_ = internal_omega;

}


// Plugin update function
void GazeboRosMotor::UpdateChild() {
    common::Time current_time = parent->GetWorld()->SimTime();
    double seconds_since_last_update = ( current_time - last_update_time_ ).Double();
    double seconds_since_last_model_update = ( current_time - last_model_update_time_ ).Double();

    double current_output_speed = joint_->GetVelocity( 0u );
    //printf("vel %f\n", current_output_speed);
    ignition::math::Vector3d current_torque = this->link_->RelativeTorque();
    double actual_load = current_torque.Z();

    if(seconds_since_last_model_update > 0.0 && seconds_since_last_model_update < 0.10) {
        motorModelUpdate(seconds_since_last_model_update, current_output_speed, actual_load);
    }
        last_model_update_time_ = current_time;


    if ( seconds_since_last_update > update_period_ ) {
        
        publishWheelJointState( current_output_speed, current_torque.Z() );
        publishMotorCurrent();
        auto dist = std::bind(std::normal_distribution<double>{current_output_speed, velocity_noise_},
                              std::mt19937(std::random_device{}()));
        double current_noisy_output_speed = dist();
        publishRotorVelocity( current_noisy_output_speed );
        publishEncoderCount( current_noisy_output_speed , seconds_since_last_update );
        last_update_time_+= common::Time ( update_period_ );
    }

    // If there was a parameter update, notify server
    paramServerUpdate();
}

// Finalize the controller
void GazeboRosMotor::FiniChild() {
    queue_.clear();
    queue_.disable();
    gazebo_ros_->node()->shutdown();
    callback_queue_thread_.join();
}

// Callback from custom que
void GazeboRosMotor::cmdVelCallback ( const std_msgs::Float32::ConstPtr& cmd_msg ) {
    input_ = cmd_msg->data;
}

void GazeboRosMotor::supplyVoltageCallBack ( const std_msgs::Float32::ConstPtr& voltage ) {
    supply_voltage_ = voltage->data;
}

void GazeboRosMotor::QueueThread() {
    static const double timeout = 0.01;
    while ( gazebo_ros_->node()->ok() ) {
        queue_.callAvailable ( ros::WallDuration ( timeout ) );
    }
}

GZ_REGISTER_MODEL_PLUGIN ( GazeboRosMotor )
// eof_ns
}
