/*
 * Copyright (C) 2016 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/
#include "ArduPilotPlugin.hh"

#include <functional>
#include <mutex>
#include <string>
#include <sstream>
#include <vector>

#include <boost/algorithm/string.hpp>

#include <sdf/sdf.hh>
#include <ignition/math/Filter.hh>
#include <gazebo/common/Assert.hh>
#include <gazebo/common/Plugin.hh>
#include <gazebo/msgs/msgs.hh>
#include <gazebo/sensors/sensors.hh>
#include <gazebo/transport/transport.hh>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "Socket.h"

// MAX_MOTORS limits the maximum number of <control> elements that
// can be defined in the <plugin>.
#define MAX_MOTORS 255

// SITL JSON interface supplies 16 servo channels
#define MAX_SERVO_CHANNELS 16

using namespace gazebo;

GZ_REGISTER_MODEL_PLUGIN(ArduPilotPlugin)

// The servo packet received from ArduPilot SITL. Defined in SIM_JSON.h.
struct servo_packet {
    uint16_t magic;         // 18458 expected magic value
    uint16_t frame_rate;
    uint32_t frame_count;
    uint16_t pwm[16];
};

/// \brief class Control is responsible for controlling a joint
class Control
{
  /// \brief Constructor
  public: Control()
  {
    // most of these coefficients are not used yet.
    this->rotorVelocitySlowdownSim = this->kDefaultRotorVelocitySlowdownSim;
    this->frequencyCutoff = this->kDefaultFrequencyCutoff;
    this->samplingRate = this->kDefaultSamplingRate;

    this->pid.Init(0.1, 0, 0, 0, 0, 1.0, -1.0);
  }

  /// \brief The PWM channel used to command this control 
  public: int channel = 0;

  /// \brief Next command to be applied to the joint
  public: double cmd = 0;

  /// \brief Velocity PID for motor control
  public: common::PID pid;

  /// \brief The controller type
  ///
  /// Valid controller types are:
  ///   VELOCITY control velocity of joint
  ///   POSITION control position of joint
  ///   EFFORT control effort of joint
  public: std::string type;

  /// \brief Use force controller
  public: bool useForce = true;

  /// \brief The name of the joint being controlled
  public: std::string jointName;

  /// \brief The joint being controlled
  public: physics::JointPtr joint;

  /// \brief A multiplier to scale the raw input command
  public: double multiplier = 1.0;

  /// \brief An offset to shift the zero-point of the raw input command
  public: double offset = 0.0;

  /// \brief Lower bound of PWM input, has default (1000).
  ///
  /// The lower bound of PWM input should match SERVOX_MIN for this channel.
  public: double servo_min = 1000.0;

  /// \brief Upper limit of PWM input, has default (2000).
  ///
  /// The upper limit of PWM input should match SERVOX_MAX for this channel.
  public: double servo_max = 2000.0;

  /// \brief unused coefficients
  public: double rotorVelocitySlowdownSim;
  public: double frequencyCutoff;
  public: double samplingRate;
  public: ignition::math::OnePole<double> filter;

  public: static double kDefaultRotorVelocitySlowdownSim;
  public: static double kDefaultFrequencyCutoff;
  public: static double kDefaultSamplingRate;
};

double Control::kDefaultRotorVelocitySlowdownSim = 10.0;
double Control::kDefaultFrequencyCutoff = 5.0;
double Control::kDefaultSamplingRate = 0.2;

/////////////////////////////////////////////////
/// \brief A wrapper class to enable Subscribe() to register the std::function callback 
template<typename M>
class OnMessageWrapper
{
public:
    typedef std::function<void(const boost::shared_ptr<M const>&)> callback_t;
    callback_t callback_;

    OnMessageWrapper(const callback_t& callback) : callback_(callback) {}

    inline void OnMessage(const boost::shared_ptr<M const> &_msg)
    {
        if (callback_)
        {
           callback_(_msg);
        }
    }
};

typedef std::shared_ptr<OnMessageWrapper<msgs::SonarStamped>> SonarOnMessageWrapperPtr;

/////////////////////////////////////////////////
// Private data class
class gazebo::ArduPilotPluginPrivate
{
  /// \brief Pointer to the update event connection.
  public: event::ConnectionPtr updateConnection;

  /// \brief Pointer to the model;
  public: physics::ModelPtr model;

  /// \brief String of the model name;
  public: std::string modelName;

  /// \brief Array of controllers
  public: std::vector<Control> controls;

  /// \brief Keep track of controller update sim-time.
  public: gazebo::common::Time lastControllerUpdateTime = 0;

  /// \brief Keep track of the time the last servo packet was received.
  public: gazebo::common::Time lastServoPacketRecvTime = 0;

  /// \brief Controller update mutex.
  public: std::mutex mutex;

  /// \brief Socket manager
  public: SocketAPM sock = SocketAPM(true);

  /// \brief The address for the flight dynamics model (i.e. this plugin)
  public: std::string fdm_address = "127.0.0.1";

  /// \brief The address for the SITL flight controller - auto detected
  public: const char* fcu_address;

  /// \brief The port for the flight dynamics model
  public: uint16_t fdm_port_in = 9002;

  /// \brief The port for the SITL flight controller - auto detected
  public: uint16_t fcu_port_out;

  /// \brief Pointer to an IMU sensor [required]
  public: sensors::ImuSensorPtr imuSensor;

  /// \brief Pointer to an GPS sensor [optional]
  public: sensors::GpsSensorPtr gpsSensor;

  /// \brief Pointer to an Rangefinder sensor [optional]
  public: sensors::RaySensorPtr rangefinderSensor;

  /// \brief Set to true when the ArduPilot flight controller is online
  ///
  /// Set to false when Gazebo starts to prevent blocking, true when
  /// the ArduPilot controller is detected and online, and false if the
  /// connection to the ArduPilot controller times out.
  public: bool arduPilotOnline;

  /// \brief Number of consecutive missed ArduPilot controller messages
  public: int connectionTimeoutCount;

  /// \brief Max number of consecutive missed ArduPilot controller messages before timeout
  public: int connectionTimeoutMaxCount;

  /// \brief Transform from model orientation to x-forward and z-up
  public: ignition::math::Pose3d modelXYZToAirplaneXForwardZDown;

  /// \brief Transform from world frame to NED frame
  public: ignition::math::Pose3d gazeboXYZToNED;

  /// \brief Last received frame rate from the ArduPilot controller
  public: uint16_t fcu_frame_rate;

  /// \brief Last received frame count from the ArduPilot controller
  public: uint32_t fcu_frame_count = -1;

  ////////// NEW NEW NEW NEW //////////
  // @TODO document and reorganise
  public: gazebo::physics::WorldPtr world;
  public: transport::NodePtr node;
  public: std::vector<transport::SubscriberPtr> sonarSubs;

  public: std::vector<SonarOnMessageWrapperPtr> sonarCbs;

  // @TODO we will also want to keep last updated info for sensor data
  public: std::vector<double> sonarRanges;
};

/////////////////////////////////////////////////
ArduPilotPlugin::ArduPilotPlugin()
  : dataPtr(new ArduPilotPluginPrivate())
{
  this->dataPtr->arduPilotOnline = false;
  this->dataPtr->connectionTimeoutCount = 0;
}

/////////////////////////////////////////////////
ArduPilotPlugin::~ArduPilotPlugin()
{
    // reset sensor subscription ptr
    for (auto&& sub : this->dataPtr->sonarSubs) {
        sub.reset();
    }
    this->dataPtr->sonarSubs.clear();

    // finalise the node and reset ptr
    // if (this->dataPtr->node) {
    //     this->dataPtr->node->Fini();
    // }
    // this->dataPtr->node.reset();

    // reset connection ptr
    this->dataPtr->updateConnection.reset();
}

/////////////////////////////////////////////////
void ArduPilotPlugin::Load(physics::ModelPtr _model, sdf::ElementPtr _sdf)
{
  GZ_ASSERT(_model, "ArduPilotPlugin _model pointer is null");
  GZ_ASSERT(_sdf, "ArduPilotPlugin _sdf pointer is null");

  this->dataPtr->model = _model;
  this->dataPtr->modelName = this->dataPtr->model->GetName();

  // modelXYZToAirplaneXForwardZDown brings us from gazebo model frame:
  // x-forward, y-right, z-down
  // to the aerospace convention: x-forward, y-left, z-up
  this->dataPtr->modelXYZToAirplaneXForwardZDown =
    ignition::math::Pose3d(0, 0, 0, 0, 0, 0);
  if (_sdf->HasElement("modelXYZToAirplaneXForwardZDown"))
  {
    this->dataPtr->modelXYZToAirplaneXForwardZDown =
        _sdf->Get<ignition::math::Pose3d>("modelXYZToAirplaneXForwardZDown");
  }

  // gazeboXYZToNED: from gazebo model frame: x-forward, y-right, z-down
  // to the aerospace convention: x-forward, y-left, z-up
  this->dataPtr->gazeboXYZToNED = ignition::math::Pose3d(0, 0, 0, IGN_PI, 0, 0);
  if (_sdf->HasElement("gazeboXYZToNED"))
  {
    this->dataPtr->gazeboXYZToNED = _sdf->Get<ignition::math::Pose3d>("gazeboXYZToNED");
  }

  // Load control channel params
  this->LoadControlChannels(_sdf);

  // Load sensor params
  this->LoadImuSensors(_sdf);
  this->LoadGpsSensors(_sdf);
  this->LoadRangeSensors(_sdf);
  this->LoadSonarSensors(_sdf);

  // Controller time control.
  this->dataPtr->lastControllerUpdateTime = 0;

  // Initialise sockets
  if (!InitSockets(_sdf))
  {
    return;
  }

  // Missed update count before we declare arduPilotOnline status false
  this->dataPtr->connectionTimeoutMaxCount =
    _sdf->Get("connectionTimeoutMaxCount", 10).first;

  // Listen to the update event. This event is broadcast every simulation
  // iteration.
  this->dataPtr->updateConnection = event::Events::ConnectWorldUpdateBegin(
      std::bind(&ArduPilotPlugin::OnUpdate, this));

  gzmsg << "[" << this->dataPtr->modelName << "] "
        << "ArduPilot ready to fly. The force will be with you" << std::endl;
}

/////////////////////////////////////////////////
void ArduPilotPlugin::LoadControlChannels(sdf::ElementPtr _sdf)
{
  // per control channel
  sdf::ElementPtr controlSDF;
  if (_sdf->HasElement("control"))
  {
    controlSDF = _sdf->GetElement("control");
  }
  else if (_sdf->HasElement("rotor"))
  {
    gzwarn << "[" << this->dataPtr->modelName << "] "
           << "please deprecate <rotor> block, use <control> block instead.\n";
    controlSDF = _sdf->GetElement("rotor");
  }

  while (controlSDF)
  {
    Control control;

    if (controlSDF->HasAttribute("channel"))
    {
      control.channel =
        atoi(controlSDF->GetAttribute("channel")->GetAsString().c_str());
    }
    else if (controlSDF->HasAttribute("id"))
    {
      gzwarn << "[" << this->dataPtr->modelName << "] "
             <<  "please deprecate attribute id, use channel instead.\n";
      control.channel =
        atoi(controlSDF->GetAttribute("id")->GetAsString().c_str());
    }
    else
    {
      control.channel = this->dataPtr->controls.size();
      gzwarn << "[" << this->dataPtr->modelName << "] "
             <<  "id/channel attribute not specified, use order parsed ["
             << control.channel << "].\n";
    }

    if (controlSDF->HasElement("type"))
    {
      control.type = controlSDF->Get<std::string>("type");
    }
    else
    {
      gzerr << "[" << this->dataPtr->modelName << "] "
            <<  "Control type not specified,"
            << " using velocity control by default.\n";
      control.type = "VELOCITY";
    }

    if (control.type != "VELOCITY" &&
        control.type != "POSITION" &&
        control.type != "EFFORT")
    {
      gzwarn << "[" << this->dataPtr->modelName << "] "
             << "Control type [" << control.type
             << "] not recognized, must be one of VELOCITY, POSITION, EFFORT."
             << " default to VELOCITY.\n";
      control.type = "VELOCITY";
    }

    if (controlSDF->HasElement("useForce"))
    {
      control.useForce = controlSDF->Get<bool>("useForce");
    }

    if (controlSDF->HasElement("jointName"))
    {
      control.jointName = controlSDF->Get<std::string>("jointName");
    }
    else
    {
      gzerr << "[" << this->dataPtr->modelName << "] "
            << "Please specify a jointName,"
            << " where the control channel is attached.\n";
    }

    // Get the pointer to the joint.
    control.joint = this->dataPtr->model->GetJoint(control.jointName);
    if (control.joint == nullptr)
    {
      gzerr << "[" << this->dataPtr->modelName << "] "
            << "Couldn't find specified joint ["
            << control.jointName << "]. This plugin will not run.\n";
      return;
    }

    if (controlSDF->HasElement("multiplier"))
    {
      // overwrite turningDirection, deprecated.
      control.multiplier = controlSDF->Get<double>("multiplier");
    }
    else if (controlSDF->HasElement("turningDirection"))
    {
      gzwarn << "[" << this->dataPtr->modelName << "] "
             << "<turningDirection> is deprecated. Please use"
             << " <multiplier>. Map 'cw' to '-1' and 'ccw' to '1'.\n";
      std::string turningDirection = controlSDF->Get<std::string>(
          "turningDirection");
      // special cases mimic from controls_gazebo_plugins
      if (turningDirection == "cw")
      {
        control.multiplier = -1;
      }
      else if (turningDirection == "ccw")
      {
        control.multiplier = 1;
      }
      else
      {
        gzdbg << "[" << this->dataPtr->modelName << "] "
              << "not string, check turningDirection as float\n";
        control.multiplier = controlSDF->Get<double>("turningDirection");
      }
    }
    else
    {
      gzdbg << "[" << this->dataPtr->modelName << "] "
            << "channel[" << control.channel
            << "]: <multiplier> (or deprecated <turningDirection>) not specified, "
            << " default to " << control.multiplier
            << " (or deprecated <turningDirection> 'ccw').\n";
    }

    if (controlSDF->HasElement("offset"))
    {
      control.offset = controlSDF->Get<double>("offset");
    }
    else
    {
      gzdbg << "[" << this->dataPtr->modelName << "] "
            << "channel[" << control.channel
            << "]: <offset> not specified, default to "
            << control.offset << "\n";
    }

    if (controlSDF->HasElement("servo_min"))
    {
      control.servo_min = controlSDF->Get<double>("servo_min");
    }
    else
    {
      gzdbg << "[" << this->dataPtr->modelName << "] "
            << "channel[" << control.channel
            << "]: <servo_min> not specified, default to "
            << control.servo_min << "\n";
    }

    if (controlSDF->HasElement("servo_max"))
    {
      control.servo_max = controlSDF->Get<double>("servo_max");
    }
    else
    {
      gzdbg << "[" << this->dataPtr->modelName << "] "
            << "channel[" << control.channel
            << "]: <servo_max> not specified, default to "
            << control.servo_max << "\n";
    }

    control.rotorVelocitySlowdownSim =
        controlSDF->Get("rotorVelocitySlowdownSim", 1).first;

    if (ignition::math::equal(control.rotorVelocitySlowdownSim, 0.0))
    {
      gzwarn << "[" << this->dataPtr->modelName << "] "
             << "control for joint [" << control.jointName
             << "] rotorVelocitySlowdownSim is zero,"
             << " assume no slowdown.\n";
      control.rotorVelocitySlowdownSim = 1.0;
    }

    control.frequencyCutoff =
          controlSDF->Get("frequencyCutoff", control.frequencyCutoff).first;
    control.samplingRate =
          controlSDF->Get("samplingRate", control.samplingRate).first;

    // use gazebo::math::Filter
    control.filter.Fc(control.frequencyCutoff, control.samplingRate);

    // initialize filter to zero value
    control.filter.Set(0.0);

    // note to use this filter, do
    // stateFiltered = filter.Process(stateRaw);

    // Overload the PID parameters if they are available.
    double param;
    // carry over from ArduCopter plugin
    param = controlSDF->Get("vel_p_gain", control.pid.GetPGain()).first;
    control.pid.SetPGain(param);

    param = controlSDF->Get("vel_i_gain", control.pid.GetIGain()).first;
    control.pid.SetIGain(param);

    param = controlSDF->Get("vel_d_gain", control.pid.GetDGain()).first;
    control.pid.SetDGain(param);

    param = controlSDF->Get("vel_i_max", control.pid.GetIMax()).first;
    control.pid.SetIMax(param);

    param = controlSDF->Get("vel_i_min", control.pid.GetIMin()).first;
    control.pid.SetIMin(param);

    param = controlSDF->Get("vel_cmd_max", control.pid.GetCmdMax()).first;
    control.pid.SetCmdMax(param);

    param = controlSDF->Get("vel_cmd_min", control.pid.GetCmdMin()).first;
    control.pid.SetCmdMin(param);

    // new params, overwrite old params if exist
    param = controlSDF->Get("p_gain", control.pid.GetPGain()).first;
    control.pid.SetPGain(param);

    param = controlSDF->Get("i_gain", control.pid.GetIGain()).first;
    control.pid.SetIGain(param);

    param = controlSDF->Get("d_gain", control.pid.GetDGain()).first;
    control.pid.SetDGain(param);

    param = controlSDF->Get("i_max", control.pid.GetIMax()).first;
    control.pid.SetIMax(param);

    param = controlSDF->Get("i_min", control.pid.GetIMin()).first;
    control.pid.SetIMin(param);

    param = controlSDF->Get("cmd_max", control.pid.GetCmdMax()).first;
    control.pid.SetCmdMax(param);

    param = controlSDF->Get("cmd_min", control.pid.GetCmdMin()).first;
    control.pid.SetCmdMin(param);

    // set pid initial command
    control.pid.SetCmd(0.0);

    this->dataPtr->controls.push_back(control);
    controlSDF = controlSDF->GetNextElement("control");
  }
}

/////////////////////////////////////////////////
void ArduPilotPlugin::LoadImuSensors(sdf::ElementPtr _sdf)
{
  std::string imuName =
    _sdf->Get("imuName", static_cast<std::string>("imu_sensor")).first;
  std::vector<std::string> imuScopedName =
    this->dataPtr->model->SensorScopedName(imuName);

  if (imuScopedName.size() > 1)
  {
    gzwarn << "[" << this->dataPtr->modelName << "] "
           << "multiple names match [" << imuName << "] using first found"
           << " name.\n";
    for (unsigned k = 0; k < imuScopedName.size(); ++k)
    {
      gzwarn << "  sensor " << k << " [" << imuScopedName[k] << "].\n";
    }
  }

  if (imuScopedName.size() > 0)
  {
    this->dataPtr->imuSensor = std::dynamic_pointer_cast<sensors::ImuSensor>
      (sensors::SensorManager::Instance()->GetSensor(imuScopedName[0]));
  }

  if (!this->dataPtr->imuSensor)
  {
    if (imuScopedName.size() > 1)
    {
      gzwarn << "[" << this->dataPtr->modelName << "] "
             << "first imu_sensor scoped name [" << imuScopedName[0]
             << "] not found, trying the rest of the sensor names.\n";
      for (unsigned k = 1; k < imuScopedName.size(); ++k)
      {
        this->dataPtr->imuSensor = std::dynamic_pointer_cast<sensors::ImuSensor>
          (sensors::SensorManager::Instance()->GetSensor(imuScopedName[k]));
        if (this->dataPtr->imuSensor)
        {
          gzwarn << "found [" << imuScopedName[k] << "]\n";
          break;
        }
      }
    }

    if (!this->dataPtr->imuSensor)
    {
      gzwarn << "[" << this->dataPtr->modelName << "] "
             << "imu_sensor scoped name [" << imuName
             << "] not found, trying unscoped name.\n" << "\n";
      // TODO: this fails for multi-nested models.
      // TODO: and transforms fail for rotated nested model,
      //       joints point the wrong way.
      this->dataPtr->imuSensor = std::dynamic_pointer_cast<sensors::ImuSensor>
        (sensors::SensorManager::Instance()->GetSensor(imuName));
    }

    if (!this->dataPtr->imuSensor)
    {
      gzerr << "[" << this->dataPtr->modelName << "] "
            << "imu_sensor [" << imuName
            << "] not found, abort ArduPilot plugin.\n" << "\n";
      return;
    }
  }
}

/////////////////////////////////////////////////
void ArduPilotPlugin::LoadGpsSensors(sdf::ElementPtr _sdf)
{
  /*
  NOT MERGED IN MASTER YET
  // Get GPS
  std::string gpsName = _sdf->Get("imuName", static_cast<std::string>("gps_sensor")).first;
  std::vector<std::string> gpsScopedName = SensorScopedName(this->dataPtr->model, gpsName);
  if (gpsScopedName.size() > 1)
  {
    gzwarn << "[" << this->dataPtr->modelName << "] "
           << "multiple names match [" << gpsName << "] using first found"
           << " name.\n";
    for (unsigned k = 0; k < gpsScopedName.size(); ++k)
    {
      gzwarn << "  sensor " << k << " [" << gpsScopedName[k] << "].\n";
    }
  }

  if (gpsScopedName.size() > 0)
  {
    this->dataPtr->gpsSensor = std::dynamic_pointer_cast<sensors::GpsSensor>
      (sensors::SensorManager::Instance()->GetSensor(gpsScopedName[0]));
  }

  if (!this->dataPtr->gpsSensor)
  {
    if (gpsScopedName.size() > 1)
    {
      gzwarn << "[" << this->dataPtr->modelName << "] "
             << "first gps_sensor scoped name [" << gpsScopedName[0]
             << "] not found, trying the rest of the sensor names.\n";
      for (unsigned k = 1; k < gpsScopedName.size(); ++k)
      {
        this->dataPtr->gpsSensor = std::dynamic_pointer_cast<sensors::GpsSensor>
          (sensors::SensorManager::Instance()->GetSensor(gpsScopedName[k]));
        if (this->dataPtr->gpsSensor)
        {
          gzwarn << "found [" << gpsScopedName[k] << "]\n";
          break;
        }
      }
    }

    if (!this->dataPtr->gpsSensor)
    {
      gzwarn << "[" << this->dataPtr->modelName << "] "
             << "gps_sensor scoped name [" << gpsName
             << "] not found, trying unscoped name.\n" << "\n";
      this->dataPtr->gpsSensor = std::dynamic_pointer_cast<sensors::GpsSensor>
        (sensors::SensorManager::Instance()->GetSensor(gpsName));
    }

    if (!this->dataPtr->gpsSensor)
    {
      gzwarn << "[" << this->dataPtr->modelName << "] "
             << "gps [" << gpsName
             << "] not found, skipping gps support.\n" << "\n";
    }
    else
    {
      gzwarn << "[" << this->dataPtr->modelName << "] "
             << "  found "  << " [" << gpsName << "].\n";
    }
  }
  */
}

/////////////////////////////////////////////////
void ArduPilotPlugin::LoadRangeSensors(sdf::ElementPtr _sdf)
{
  /*
  // Get Rangefinder
  // TODO add sonar
  std::string rangefinderName = _sdf->Get("rangefinderName",
    static_cast<std::string>("rangefinder_sensor")).first;
  std::vector<std::string> rangefinderScopedName = SensorScopedName(this->dataPtr->model, rangefinderName);
  if (rangefinderScopedName.size() > 1)
  {
    gzwarn << "[" << this->dataPtr->modelName << "] "
           << "multiple names match [" << rangefinderName << "] using first found"
           << " name.\n";
    for (unsigned k = 0; k < rangefinderScopedName.size(); ++k)
    {
      gzwarn << "  sensor " << k << " [" << rangefinderScopedName[k] << "].\n";
    }
  }

  if (rangefinderScopedName.size() > 0)
  {
    this->dataPtr->rangefinderSensor = std::dynamic_pointer_cast<sensors::RaySensor>
      (sensors::SensorManager::Instance()->GetSensor(rangefinderScopedName[0]));
  }

  if (!this->dataPtr->rangefinderSensor)
  {
    if (rangefinderScopedName.size() > 1)
    {
      gzwarn << "[" << this->dataPtr->modelName << "] "
             << "first rangefinder_sensor scoped name [" << rangefinderScopedName[0]
             << "] not found, trying the rest of the sensor names.\n";
      for (unsigned k = 1; k < rangefinderScopedName.size(); ++k)
      {
        this->dataPtr->rangefinderSensor = std::dynamic_pointer_cast<sensors::RaySensor>
          (sensors::SensorManager::Instance()->GetSensor(rangefinderScopedName[k]));
        if (this->dataPtr->rangefinderSensor)
        {
          gzwarn << "found [" << rangefinderScopedName[k] << "]\n";
          break;
        }
      }
    }

    if (!this->dataPtr->rangefinderSensor)
    {
      gzwarn << "[" << this->dataPtr->modelName << "] "
             << "rangefinder_sensor scoped name [" << rangefinderName
             << "] not found, trying unscoped name.\n" << "\n";
      /// TODO: this fails for multi-nested models.
      /// TODO: and transforms fail for rotated nested model,
      ///       joints point the wrong way.
      this->dataPtr->rangefinderSensor = std::dynamic_pointer_cast<sensors::RaySensor>
        (sensors::SensorManager::Instance()->GetSensor(rangefinderName));
    }
    if (!this->dataPtr->rangefinderSensor)
    {
      gzwarn << "[" << this->dataPtr->modelName << "] "
             << "ranfinder [" << rangefinderName
             << "] not found, skipping rangefinder support.\n" << "\n";
    }
    else
    {
      gzwarn << "[" << this->dataPtr->modelName << "] "
             << "  found "  << " [" << rangefinderName << "].\n";
    }
  }
  */
}

/////////////////////////////////////////////////
void ArduPilotPlugin::LoadSonarSensors(sdf::ElementPtr _sdf)
{
    struct SensorIdentifier {
        std::string type;
        int index;
        std::string topic;
    };
    std::vector<SensorIdentifier> sensorIds;

    // read sensor elements
    sdf::ElementPtr sensorSdf;
    if (_sdf->HasElement("sensor")) {
        sensorSdf = _sdf->GetElement("sensor");
    }

    while (sensorSdf) {
        SensorIdentifier sensorId;

        // <type> is required
        if (sensorSdf->HasElement("type")) {
            sensorId.type = sensorSdf->Get<std::string>("type");
        } else {
            gzerr << "[" << this->dataPtr->modelName << "] "
                <<  "sensor element 'type' not specified, skipping.\n";
        } 

        // <index> is required
        if (sensorSdf->HasElement("index")) {
            sensorId.index = sensorSdf->Get<int>("index");
        } else {
            gzerr << "[" << this->dataPtr->modelName << "] "
                <<  "sensor element 'index' not specified, skipping.\n";
        } 

        // <topic> is required
        if (sensorSdf->HasElement("topic")) {
            sensorId.topic = sensorSdf->Get<std::string>("topic");
        } else {
            gzerr << "[" << this->dataPtr->modelName << "] "
                <<  "sensor element 'topic' not specified, skipping.\n";
        } 

        sensorIds.push_back(sensorId);

        sensorSdf = sensorSdf->GetNextElement("sensor");

        gzdbg << "[" << this->dataPtr->modelName << "] sonar "
            << "type: " << sensorId.type
            << ", index: " << sensorId.index
            << ", topic: " << sensorId.topic
            << "\n";
    }

    // @TODO - associate each subscription with an index for the sensors
    //       - could use bind to populate an additional argument...


    // @TODO - move to Load
    // world ptr
    this->dataPtr->world = this->dataPtr->model->GetWorld();

    // @TODO - move to Ctor and Load
    // create and initialise transport node
    this->dataPtr->node = transport::NodePtr(new transport::Node());
    this->dataPtr->node->Init(this->dataPtr->world->Name());

    // @TODO - move to own function
    // get the topic prefix
    std::string topicPrefix = "~/";
    topicPrefix += this->dataPtr->modelName;
    boost::replace_all(topicPrefix, "::", "/");

    // subscriptions
    for (auto&& sensorId : sensorIds) {
        // @TODO - move to own function
        // fully qualified topic name
        std::string topicName = topicPrefix;
        topicName.append("/").append(sensorId.topic);

        // bind the sensor index to the callback function (adjust from unit to zero offset) 
        OnMessageWrapper<msgs::SonarStamped>::callback_t fn = std::bind(
            &ArduPilotPlugin::OnSonarStamped, this,
            std::placeholders::_1,
            sensorId.index - 1);

        // wrap the std::function so we can register the callback
        auto callbackWrapper = SonarOnMessageWrapperPtr(
            new OnMessageWrapper<msgs::SonarStamped>(fn));

        auto callback = &OnMessageWrapper<msgs::SonarStamped>::OnMessage;

        // subscribe to sonar sensor topic
        transport::SubscriberPtr sonarSub =
            this->dataPtr->node->Subscribe<msgs::SonarStamped>(
                topicName, callback, callbackWrapper.get());

        this->dataPtr->sonarSubs.push_back(sonarSub);
        this->dataPtr->sonarCbs.push_back(callbackWrapper);

        // @TODO initalise ranges properly (AP convention for ignored value?)
        this->dataPtr->sonarRanges.push_back(-1.0);

        gzdbg << "[" << this->dataPtr->modelName << "] subscribing to "
            << topicName << "\n";
   }

}

/////////////////////////////////////////////////
void ArduPilotPlugin::OnUpdate()
{
  std::lock_guard<std::mutex> lock(this->dataPtr->mutex);

  const gazebo::common::Time curTime = this->dataPtr->model->GetWorld()->SimTime();
  double dt = (curTime - this->dataPtr->lastControllerUpdateTime).Double();
  this->dataPtr->lastControllerUpdateTime = curTime;
 
  // Update the control surfaces and publish the new state.
  if (dt > 0)
  {
    if (this->ReceiveServoPacket())
    {
        double dt_pkt = (curTime - this->dataPtr->lastServoPacketRecvTime).Double();        
        this->dataPtr->lastServoPacketRecvTime = curTime;

        // debug: synchonisation checks
#if 0
        gzdbg << "fdm_frame_rate: " << 1/dt
          << ", fcu_frame_rate: " << this->dataPtr->fcu_frame_rate
          << "\n";
        gzdbg << "fdm_frame_rate: " << 1/dt
          << ", servo_packet_rate: " << 1/dt_pkt
          << "\n";
#endif
    }

    if (this->dataPtr->arduPilotOnline)
    {
      this->ApplyMotorForces(dt);    
      this->SendState();
    }
  }
}

/////////////////////////////////////////////////
void ArduPilotPlugin::ResetPIDs()
{
  // Reset velocity PID for controls
  for (size_t i = 0; i < this->dataPtr->controls.size(); ++i)
  {
    this->dataPtr->controls[i].cmd = 0;
    // this->dataPtr->controls[i].pid.Reset();
  }
}

/////////////////////////////////////////////////
bool ArduPilotPlugin::InitSockets(sdf::ElementPtr _sdf) const
{   
    // configure port
    this->dataPtr->sock.set_blocking(false);
    this->dataPtr->sock.reuseaddress();

    // get the fdm address if provided, otherwise default to localhost
    this->dataPtr->fdm_address =
        _sdf->Get("fdm_addr", static_cast<std::string>("127.0.0.1")).first;

    this->dataPtr->fdm_port_in =
        _sdf->Get("fdm_port_in", static_cast<uint32_t>(9002)).first;

    // output port configuration is automatic
    if (_sdf->HasElement("listen_addr")) {
        gzwarn << "Param <listen_addr> is deprecated, connection is auto detected\n";
    }
    if (_sdf->HasElement("fdm_port_out")) {
        gzwarn << "Param <fdm_port_out> is deprecated, connection is auto detected\n";
    }

    // bind the socket
    if (!this->dataPtr->sock.bind(this->dataPtr->fdm_address.c_str(), this->dataPtr->fdm_port_in))
    {
        gzerr << "[" << this->dataPtr->modelName << "] "
            << "failed to bind with "
            << this->dataPtr->fdm_address << ":" << this->dataPtr->fdm_port_in
            << " aborting plugin.\n";
        return false;
    }
    gzmsg << "[" << this->dataPtr->modelName << "] "
        << "flight dynamics model @ "
        << this->dataPtr->fdm_address << ":" << this->dataPtr->fdm_port_in
        << "\n";
    return true;
}

/////////////////////////////////////////////////
void ArduPilotPlugin::ApplyMotorForces(const double _dt)
{
  // update velocity PID for controls and apply force to joint
  for (size_t i = 0; i < this->dataPtr->controls.size(); ++i)
  {
    if (this->dataPtr->controls[i].useForce)
    {
      if (this->dataPtr->controls[i].type == "VELOCITY")
      {
        const double velTarget = this->dataPtr->controls[i].cmd /
          this->dataPtr->controls[i].rotorVelocitySlowdownSim;
        const double vel = this->dataPtr->controls[i].joint->GetVelocity(0);
        const double error = vel - velTarget;
        const double force = this->dataPtr->controls[i].pid.Update(error, _dt);
        this->dataPtr->controls[i].joint->SetForce(0, force);
      }
      else if (this->dataPtr->controls[i].type == "POSITION")
      {
        const double posTarget = this->dataPtr->controls[i].cmd;
        const double pos = this->dataPtr->controls[i].joint->Position();
        const double error = pos - posTarget;
        const double force = this->dataPtr->controls[i].pid.Update(error, _dt);
        this->dataPtr->controls[i].joint->SetForce(0, force);
      }
      else if (this->dataPtr->controls[i].type == "EFFORT")
      {
        const double force = this->dataPtr->controls[i].cmd;
        this->dataPtr->controls[i].joint->SetForce(0, force);
      }
      else
      {
        // do nothing
      }
    }
    else
    {
      if (this->dataPtr->controls[i].type == "VELOCITY")
      {
        this->dataPtr->controls[i].joint->SetVelocity(0, this->dataPtr->controls[i].cmd);
      }
      else if (this->dataPtr->controls[i].type == "POSITION")
      {
        this->dataPtr->controls[i].joint->SetPosition(0, this->dataPtr->controls[i].cmd);
      }
      else if (this->dataPtr->controls[i].type == "EFFORT")
      {
        const double force = this->dataPtr->controls[i].cmd;
        this->dataPtr->controls[i].joint->SetForce(0, force);
      }
      else
      {
        // do nothing
      }
    }
  }
}

/////////////////////////////////////////////////
bool ArduPilotPlugin::ReceiveServoPacket()
{
    // Added detection for whether ArduPilot is online or not.
    // If ArduPilot is detected (receive of fdm packet from someone),
    // then socket receive wait time is increased from 1ms to 1 sec
    // to accomodate network jitter.
    // If ArduPilot is not detected, receive call blocks for 1ms
    // on each call.
    // Once ArduPilot presence is detected, it takes this many
    // missed receives before declaring the FCS offline.

    uint32_t waitMs;
    if (this->dataPtr->arduPilotOnline)
    {
        // Increase timeout for recv once we detect a packet from ArduPilot FCS.
        // If this value is too high then it will block the main Gazebo update loop
        // and adversely affect the RTF.
        waitMs = 10;
    }
    else
    {
        // Otherwise skip quickly and do not set control force.
        waitMs = 1;
    }

    servo_packet pkt;
    auto recvSize = this->dataPtr->sock.recv(&pkt, sizeof(servo_packet), waitMs);
  
    this->dataPtr->sock.last_recv_address(this->dataPtr->fcu_address, this->dataPtr->fcu_port_out);

    // drain the socket in the case we're backed up
    int counter = 0;
    while (true)
    {
        servo_packet last_pkt;
        auto recvSize_last = this->dataPtr->sock.recv(&last_pkt, sizeof(servo_packet), 0ul);
        if (recvSize_last == -1)
        {
            break;
        }
        counter++;
        pkt = last_pkt;
        recvSize = recvSize_last;
    }
    if (counter > 0)
    {
        gzwarn << "[" << this->dataPtr->modelName << "] "
            << "Drained n packets: " << counter << std::endl;
    }

    // didn't receive a packet, increment timeout count if online, then return
    if (recvSize == -1)
    {
        if (this->dataPtr->arduPilotOnline)
        {
            if (++this->dataPtr->connectionTimeoutCount >
            this->dataPtr->connectionTimeoutMaxCount)
            {
                this->dataPtr->connectionTimeoutCount = 0;
                this->dataPtr->arduPilotOnline = false;
                gzwarn << "[" << this->dataPtr->modelName << "] "
                    << "Broken ArduPilot connection, resetting motor control.\n";
                this->ResetPIDs();
            }
        }
        return false;
    }

#if 0
    // debug: inspect sitl packet
    std::ostringstream oss;
    oss << "recv " << recvSize << " bytes from "
        << this->dataPtr->fcu_address << ":" << this->dataPtr->fcu_port_out << "\n";
    oss << "magic: " << pkt.magic << "\n";
    oss << "frame_rate: " << pkt.frame_rate << "\n";
    oss << "frame_count: " << pkt.frame_count << "\n";
    oss << "pwm: [";
    for (auto i=0; i<MAX_SERVO_CHANNELS - 1; ++i) {
        oss << pkt.pwm[i] << ", ";
    }
    oss << pkt.pwm[MAX_SERVO_CHANNELS - 1] << "]\n";
    gzdbg << "\n" << oss.str();
#endif

    // check magic, return if invalid
    const uint16_t magic = 18458;
    if (magic != pkt.magic)
    {
        gzwarn << "Incorrect protocol magic "
            << pkt.magic << " should be "
            << magic << "\n";
        return false;
    }

    // check frame rate and frame order
    this->dataPtr->fcu_frame_rate = pkt.frame_rate;
    if (pkt.frame_count < this->dataPtr->fcu_frame_count)
    {
        // @TODO - implement re-initialisation 
        gzwarn << "ArduPilot controller has reset\n";
    }
    else if (pkt.frame_count == this->dataPtr->fcu_frame_count)
    {
        // received duplicate frame, skip
        gzwarn << "Duplicate input frame\n";
        return false;
    }
    else if (pkt.frame_count != this->dataPtr->fcu_frame_count + 1
        && this->dataPtr->arduPilotOnline)
    {
        // missed frames, warn only
        gzwarn << "Missed "
            << this->dataPtr->fcu_frame_count - pkt.frame_count
            << " input frames\n";
    }
    this->dataPtr->fcu_frame_count = pkt.frame_count;

    // always reset the connection timeout so we don't accumulate
    this->dataPtr->connectionTimeoutCount = 0;
    if (!this->dataPtr->arduPilotOnline)
    {
        this->dataPtr->arduPilotOnline = true;

        gzmsg << "[" << this->dataPtr->modelName << "] "
            << "Connected to ArduPilot controller @ "
            << this->dataPtr->fcu_address << ":" << this->dataPtr->fcu_port_out
            << "\n";
    }

    // compute command based on requested motorSpeed
    for (unsigned i = 0; i < this->dataPtr->controls.size(); ++i)
    {
        // enforce limit on the number of <control> elements
        if (i < MAX_MOTORS)
        {
            if (this->dataPtr->controls[i].channel < MAX_SERVO_CHANNELS)
            {
                // convert pwm to raw cmd: [servo_min, servo_max] => [0, 1],
                // default is: [1000, 2000] => [0, 1]
                const double pwm = pkt.pwm[this->dataPtr->controls[i].channel];
                const double pwm_min = this->dataPtr->controls[i].servo_min;
                const double pwm_max = this->dataPtr->controls[i].servo_max;
                const double multiplier = this->dataPtr->controls[i].multiplier;
                const double offset = this->dataPtr->controls[i].offset;

                // bound incoming cmd between 0 and 1
                double raw_cmd = (pwm - pwm_min)/(pwm_max - pwm_min);
                raw_cmd = ignition::math::clamp(raw_cmd, 0.0, 1.0);
                this->dataPtr->controls[i].cmd = multiplier * (raw_cmd + offset);

#if 0
                gzdbg << "apply input chan[" << this->dataPtr->controls[i].channel
                    << "] to control chan[" << i
                    << "] with joint name ["
                    << this->dataPtr->controls[i].jointName
                    << "] pwm [" << pwm
                    << "] raw cmd [" << raw_cmd
                    << "] adjusted cmd [" << this->dataPtr->controls[i].cmd
                    << "].\n";
#endif
            }
            else
            {
                gzerr << "[" << this->dataPtr->modelName << "] "
                    << "control[" << i << "] channel ["
                    << this->dataPtr->controls[i].channel
                    << "] is greater than the number of servo channels ["
                    << MAX_SERVO_CHANNELS
                    << "], control not applied.\n";
            }
        }
        else
        {
        gzerr << "[" << this->dataPtr->modelName << "] "
                << "too many motors, skipping [" << i
                << " > " << MAX_MOTORS << "].\n";
        }
    }
    return true;
}

/////////////////////////////////////////////////
void ArduPilotPlugin::SendState() const
{
    // it is assumed that the imu orientation is:
    //   x forward
    //   y right
    //   z down

    // get linear acceleration in body frame
    const ignition::math::Vector3d linearAccel =
        this->dataPtr->imuSensor->LinearAcceleration();
    // gzdbg << "lin accel [" << linearAccel << "]\n";

    // get angular velocity in body frame
    const ignition::math::Vector3d angularVel =
        this->dataPtr->imuSensor->AngularVelocity();

    // get inertial pose and velocity
    // position of the uav in world frame
    // this position is used to calculate bearing and distance
    // from starting location, then use that to update gps position.
    // The algorithm looks something like below (from ardupilot helper
    // libraries):
    //   bearing = to_degrees(atan2(position.y, position.x));
    //   distance = math.sqrt(self.position.x**2 + self.position.y**2)
    //   (self.latitude, self.longitude) = util.gps_newpos(
    //    self.home_latitude, self.home_longitude, bearing, distance)
    // where xyz is in the NED directions.
    // Gazebo world xyz is assumed to be N, -E, -D, so flip some stuff
    // around.
    // orientation of the uav in world NED frame -
    // assuming the world NED frame has xyz mapped to NED,
    // imuLink is NED - z down
    // 
    // model world pose brings us to model,
    // which for example zephyr has -y-forward, x-left, z-up
    // adding modelXYZToAirplaneXForwardZDown rotates
    //   from: model XYZ
    //   to: airplane x-forward, y-left, z-down
    const ignition::math::Pose3d gazeboXYZToModelXForwardZDown =
    this->dataPtr->modelXYZToAirplaneXForwardZDown +
    this->dataPtr->model->WorldPose();

    // get transform from world NED to Model frame
    const ignition::math::Pose3d NEDToModelXForwardZUp =
        gazeboXYZToModelXForwardZDown - this->dataPtr->gazeboXYZToNED;
    // gzdbg << "ned to model [" << NEDToModelXForwardZUp << "]\n";

    // imuOrientationQuat is the rotation from world NED frame
    // to the uav frame.
    // gzdbg << "imu [" << gazeboXYZToModelXForwardZDown.rot.GetAsEuler()
    //       << "]\n";
    // gzdbg << "ned [" << this->gazeboXYZToNED.rot.GetAsEuler() << "]\n";
    // gzdbg << "rot [" << NEDToModelXForwardZUp.rot.GetAsEuler() << "]\n";

    // Get NED velocity in body frame *
    // or...
    // Get model velocity in NED frame
    const ignition::math::Vector3d velGazeboWorldFrame =
    this->dataPtr->model->GetLink()->WorldLinearVel();
    const ignition::math::Vector3d velNEDFrame =
    this->dataPtr->gazeboXYZToNED.Rot().RotateVectorReverse(velGazeboWorldFrame);

    // require the duration since sim start in seconds 
    double timestamp = this->dataPtr->model->GetWorld()->SimTime().Double();

    using namespace rapidjson;

    // build JSON document
    StringBuffer s;
    Writer<StringBuffer> writer(s);            

    writer.StartObject();

    writer.Key("timestamp");
    writer.Double(timestamp);

    writer.Key("imu");
    writer.StartObject();
    writer.Key("gyro");
    writer.StartArray();
    writer.Double(angularVel.X());
    writer.Double(angularVel.Y());
    writer.Double(angularVel.Z());
    writer.EndArray();
    writer.Key("accel_body");
    writer.StartArray();
    writer.Double(linearAccel.X());
    writer.Double(linearAccel.Y());
    writer.Double(linearAccel.Z());
    writer.EndArray();
    writer.EndObject();

    writer.Key("position");
    writer.StartArray();
    writer.Double(NEDToModelXForwardZUp.Pos().X());
    writer.Double(NEDToModelXForwardZUp.Pos().Y());
    writer.Double(NEDToModelXForwardZUp.Pos().Z());
    writer.EndArray();

    // ArduPilot quaternion convention: q[0] = 1 for identity.
    writer.Key("quaternion");
    writer.StartArray();
    writer.Double(NEDToModelXForwardZUp.Rot().W());
    writer.Double(NEDToModelXForwardZUp.Rot().X());
    writer.Double(NEDToModelXForwardZUp.Rot().Y());
    writer.Double(NEDToModelXForwardZUp.Rot().Z());
    writer.EndArray();

    writer.Key("velocity");
    writer.StartArray();
    writer.Double(velNEDFrame.X());
    writer.Double(velNEDFrame.Y());
    writer.Double(velNEDFrame.Z());
    writer.EndArray();

    // SITL/SIM_JSON supports these additional sensor fields
    //      rng_1 : 0 
    //      rng_2 : 0
    //      rng_3 : 0
    //      rng_4 : 0
    //      rng_5 : 0
    //      rng_6 : 0
    //      windvane : { direction: 0, speed: 0 }

    // @TODO this makes a strong assumption all range
    //       sensors with index less than sonarRanges.size()
    //       are active

    // Use case fall through to set each range sensor
    switch (this->dataPtr->sonarRanges.size())
    {
    case 10:
    case 9:
    case 8:
    case 7:
    case 6:
        writer.Key("rng_6");
        writer.Double(this->dataPtr->sonarRanges[5]);
    case 5:
        writer.Key("rng_5");
        writer.Double(this->dataPtr->sonarRanges[4]);
    case 4:
        writer.Key("rng_4");
        writer.Double(this->dataPtr->sonarRanges[3]);
    case 3:
        writer.Key("rng_3");
        writer.Double(this->dataPtr->sonarRanges[2]);
    case 2:
        writer.Key("rng_2");
        writer.Double(this->dataPtr->sonarRanges[1]);
    case 1:
        writer.Key("rng_1");
        writer.Double(this->dataPtr->sonarRanges[0]);
    default:
        break;
    }

    // writer.Key("windvane");
    // writer.StartObject();
    // writer.Key("direction");
    // writer.Double(1.57079633);
    // writer.Key("speed");
    // writer.Double(5.5);
    // writer.EndObject();

    writer.EndObject();

    // send JSON
    std::string json_str = "\n" + std::string(s.GetString()) + "\n";
    auto bytes_sent = this->dataPtr->sock.sendto(
        json_str.c_str(), json_str.size(),
        this->dataPtr->fcu_address,
        this->dataPtr->fcu_port_out);
    
    // gzdbg << "sent " << bytes_sent <<  " bytes to " 
    //     << this->dataPtr->fcu_address << ":" << this->dataPtr->fcu_port_out << "\n";
    // gzdbg << json_str << "\n";
}


/////////////////////////////////////////////////
/// \brief Handle sensor messages
///
/// Message structure:
///     gazebo/msgs/time.proto
///     gazebo/msgs/sonar.proto
///     gazebo/msgs/sonar_stamped.proto
///
void ArduPilotPlugin::OnSonarStamped(ConstSonarStampedPtr &_sonarMsg, int _sensorIndex)
{
    // extract data
    auto time_sec = _sonarMsg->time().sec();
    auto time_nsec = _sonarMsg->time().nsec();
    auto frame = _sonarMsg->sonar().frame();
    auto world_pose = _sonarMsg->sonar().world_pose();
    auto range_min = _sonarMsg->sonar().range_min();
    auto range_max = _sonarMsg->sonar().range_max();
    auto radius = _sonarMsg->sonar().radius();
    auto range = _sonarMsg->sonar().range();
    auto contact = _sonarMsg->sonar().contact();
    auto geometry = _sonarMsg->sonar().geometry();

    // Set the range for this sensor
    this->dataPtr->sonarRanges[_sensorIndex] = range;

    // gzdbg << "Sonar\n"
    //     << "index: " << _sensorIndex << "\n"
    //     << "frame: " << frame << "\n"
    //     << "range_min: " << range_min << "\n"
    //     << "range_max: " << range_max << "\n"
    //     << "range: " << range << "\n"
    //     << "radius: " << radius << "\n"
    //     << "geometry: " << geometry << "\n"
    //     << "contact: "
    //         << contact.x() << ", "
    //         << contact.y() << ", "
    //         << contact.z()
    //     << "\n";
}
