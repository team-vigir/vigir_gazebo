/*
 * Copyright 2014 Open Source Robotics Foundation
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

#include <atlas_msgs/SModelRobotInput.h>
#include <atlas_msgs/SModelRobotOutput.h>
#include <ros/ros.h>
#include <string>
#include <vector>
#include <gazebo/common/Plugin.hh>
#include <gazebo/common/Time.hh>
#include <gazebo/math/Angle.hh>
#include <gazebo/physics/physics.hh>
#include <vigir_gazebo_ros_plugins/VigirRobotiqHandPlugin.h>

// Default topic names initialization.
const std::string VigirRobotiqHandPlugin::DefaultLeftTopicCommand  =
  "/left_hand/command";
const std::string VigirRobotiqHandPlugin::DefaultLeftTopicState    =
  "/left_hand/state";
const std::string VigirRobotiqHandPlugin::DefaultRightTopicCommand =
  "/right_hand/command";
const std::string VigirRobotiqHandPlugin::DefaultRightTopicState   =
  "/right_hand/state";

////////////////////////////////////////////////////////////////////////////////
VigirRobotiqHandPlugin::VigirRobotiqHandPlugin()
{
  // PID default parameters.
  for (int i = 0; i < this->NumJoints; ++i)
  {
    this->posePID[i].Init(1.0, 0, 0.5, 0.0, 0.0, 60.0, -60.0);
    this->posePID[i].SetCmd(0.0);
  }

  // Default grasping mode: Basic mode.
  this->graspingMode = Basic;

  // Default hand state: Disabled.
  this->handState = Disabled;
}

////////////////////////////////////////////////////////////////////////////////
VigirRobotiqHandPlugin::~VigirRobotiqHandPlugin()
{
  gazebo::event::Events::DisconnectWorldUpdateBegin(this->updateConnection);
  this->rosNode->shutdown();
  this->rosQueue.clear();
  this->rosQueue.disable();
  this->callbackQueueThread.join();
}

////////////////////////////////////////////////////////////////////////////////
void VigirRobotiqHandPlugin::Load(gazebo::physics::ModelPtr _parent,
                             sdf::ElementPtr _sdf)
{
  this->model = _parent;
  this->world = this->model->GetWorld();
  this->sdf = _sdf;

  if (!this->sdf->HasElement("side") ||
      !this->sdf->GetElement("side")->GetValue()->Get(this->side) ||
      ((this->side != "left") && (this->side != "right")))
  {
    gzerr << "Failed to determine which hand we're controlling; "
             "aborting plugin load. <Side> should be either 'left' or 'right'."
          << std::endl;
    return;
  }

  // Load the vector of all joints.
  std::string prefix;
  if (this->side == "left")
    prefix = "left_";
  else
    prefix = "right_";

  // Load the vector of all joints.
  if (!this->FindJoints())
    return;

  // Initialize joint state vector.
  this->jointStates.name.resize(this->jointNames.size());
  this->jointStates.position.resize(this->jointNames.size());
  this->jointStates.velocity.resize(this->jointNames.size());
  this->jointStates.effort.resize(this->jointNames.size());
  for (size_t i = 0; i < this->jointNames.size(); ++i)
  {
    this->jointStates.name[i] = this->jointNames[i];
    this->jointStates.position[i] = 0;
    this->jointStates.velocity[i] = 0;
    this->jointStates.effort[i] = 0;
  }

  // Default ROS topic names.
  std::string controlTopicName = this->DefaultLeftTopicCommand;
  std::string stateTopicName   = this->DefaultLeftTopicState;
  if (this->side == "right")
  {
    controlTopicName = this->DefaultRightTopicCommand;
    stateTopicName   = this->DefaultRightTopicState;
  }

  for (int i = 0; i < this->NumJoints; ++i)
  {
    // Set the PID effort limits.
    this->posePID[i].SetCmdMin(-this->fingerJoints[i]->GetEffortLimit(0));
    this->posePID[i].SetCmdMax(this->fingerJoints[i]->GetEffortLimit(0));

    // Overload the PID parameters if they are available.
    if (this->sdf->HasElement("kp_position"))
      this->posePID[i].SetPGain(this->sdf->Get<double>("kp_position"));

    if (this->sdf->HasElement("ki_position"))
      this->posePID[i].SetIGain(this->sdf->Get<double>("ki_position"));

    if (this->sdf->HasElement("kd_position"))
    {
      this->posePID[i].SetDGain(this->sdf->Get<double>("kd_position"));
      std::cout << "dGain after overloading: " << this->posePID[i].GetDGain()
                << std::endl;
    }

    if (this->sdf->HasElement("position_effort_min"))
      this->posePID[i].SetCmdMin(this->sdf->Get<double>("position_effort_min"));

    if (this->sdf->HasElement("position_effort_max"))
      this->posePID[i].SetCmdMax(this->sdf->Get<double>("position_effort_max"));
  }

  // Overload the ROS topics for the hand if they are available.
  if (this->sdf->HasElement("topic_command"))
    controlTopicName = this->sdf->Get<std::string>("topic_command");

  if (this->sdf->HasElement("topic_state"))
    stateTopicName = this->sdf->Get<std::string>("topic_state");

  // Initialize ROS.
  if (!ros::isInitialized())
  {
    gzerr << "Not loading plugin since ROS hasn't been "
          << "properly initialized. Try starting gazebo with ROS plugin:\n"
          << " gazebo -s libgazebo_ros_api_plugin.so\n";
    return;
  }

  // Create a ROS node.
  this->rosNode.reset(new ros::NodeHandle(""));

  // Publish multi queue.
  this->pmq.startServiceThread();

  // Broadcasts state.
  this->pubHandleStateQueue = this->pmq.addPub<atlas_msgs::SModelRobotInput>();
  this->pubHandleState = this->rosNode->advertise<atlas_msgs::SModelRobotInput>(
    stateTopicName, 100, true);

  // Broadcast joint state.
  std::string topicBase = std::string("robotiq_hands/") + this->side;
  this->pubJointStatesQueue = this->pmq.addPub<sensor_msgs::JointState>();
  this->pubJointStates = this->rosNode->advertise<sensor_msgs::JointState>(
    topicBase + std::string("_hand/joint_states"), 10);

  // Subscribe to user published handle control commands.
  ros::SubscribeOptions handleCommandSo =
    ros::SubscribeOptions::create<atlas_msgs::SModelRobotOutput>(
      controlTopicName, 100,
      boost::bind(&VigirRobotiqHandPlugin::SetHandleCommand, this, _1),
      ros::VoidPtr(), &this->rosQueue);

  // Enable TCP_NODELAY since TCP causes bursty communication with high jitter.
  handleCommandSo.transport_hints =
    ros::TransportHints().reliable().tcpNoDelay(true);
  this->subHandleCommand = this->rosNode->subscribe(handleCommandSo);

  // Controller time control.
  this->lastControllerUpdateTime = this->world->GetSimTime();

  // Start callback queue.
  this->callbackQueueThread =
    boost::thread(boost::bind(&VigirRobotiqHandPlugin::RosQueueThread, this));

  // Connect to gazebo world update.
  this->updateConnection =
    gazebo::event::Events::ConnectWorldUpdateBegin(
      boost::bind(&VigirRobotiqHandPlugin::UpdateStates, this));

  // Log information.
  gzlog << "VigirRobotiqHandPlugin loaded for " << this->side << " hand."
        << std::endl;
  for (int i = 0; i < this->NumJoints; ++i)
  {
    gzlog << "Position PID parameters for joint ["
          << this->fingerJoints[i]->GetName() << "]:"     << std::endl
          << "\tKP: "     << this->posePID[i].GetPGain()  << std::endl
          << "\tKI: "     << this->posePID[i].GetIGain()  << std::endl
          << "\tKD: "     << this->posePID[i].GetDGain()  << std::endl
          << "\tIMin: "   << this->posePID[i].GetIMin()   << std::endl
          << "\tIMax: "   << this->posePID[i].GetIMax()   << std::endl
          << "\tCmdMin: " << this->posePID[i].GetCmdMin() << std::endl
          << "\tCmdMax: " << this->posePID[i].GetCmdMax() << std::endl
          << std::endl;
  }
  gzlog << "Topic for sending hand commands: ["   << controlTopicName
        << "]\nTopic for receiving hand state: [" << stateTopicName
        << "]" << std::endl;
}

////////////////////////////////////////////////////////////////////////////////
bool VigirRobotiqHandPlugin::VerifyField(const std::string &_label, int _min,
  int _max, int _v)
{
  if (_v < _min || _v > _max)
  {
    std::cerr << "Illegal " << _label << " value: [" << _v << "]. The correct "
              << "range is [" << _min << "," << _max << "]" << std::endl;
    return false;
  }
  return true;
}

////////////////////////////////////////////////////////////////////////////////
bool VigirRobotiqHandPlugin::VerifyCommand(
    const atlas_msgs::SModelRobotOutput::ConstPtr &_command)
{
  return this->VerifyField("rACT", 0, 1,   _command->rACT) &&
         this->VerifyField("rMOD", 0, 3,   _command->rACT) &&
         this->VerifyField("rGTO", 0, 1,   _command->rACT) &&
         this->VerifyField("rATR", 0, 1,   _command->rACT) &&
         this->VerifyField("rICF", 0, 1,   _command->rACT) &&
         this->VerifyField("rICS", 0, 1,   _command->rACT) &&
         this->VerifyField("rPRA", 0, 255, _command->rACT) &&
         this->VerifyField("rSPA", 0, 255, _command->rACT) &&
         this->VerifyField("rFRA", 0, 255, _command->rACT) &&
         this->VerifyField("rPRB", 0, 255, _command->rACT) &&
         this->VerifyField("rSPB", 0, 255, _command->rACT) &&
         this->VerifyField("rFRB", 0, 255, _command->rACT) &&
         this->VerifyField("rPRC", 0, 255, _command->rACT) &&
         this->VerifyField("rSPC", 0, 255, _command->rACT) &&
         this->VerifyField("rFRC", 0, 255, _command->rACT) &&
         this->VerifyField("rPRS", 0, 255, _command->rACT) &&
         this->VerifyField("rSPS", 0, 255, _command->rACT) &&
         this->VerifyField("rFRS", 0, 255, _command->rACT);
}

////////////////////////////////////////////////////////////////////////////////
void VigirRobotiqHandPlugin::SetHandleCommand(
    const atlas_msgs::SModelRobotOutput::ConstPtr &_msg)
{
  boost::mutex::scoped_lock lock(this->controlMutex);

  // Sanity check.
  if (!this->VerifyCommand(_msg))
  {
    std::cerr << "Ignoring command" << std::endl;
    return;
  }

  this->prevCommand = this->handleCommand;

  // Update handleCommand.
  this->handleCommand = *_msg;
}

////////////////////////////////////////////////////////////////////////////////
void VigirRobotiqHandPlugin::ReleaseHand()
{
  // Open the fingers.
  this->handleCommand.rPRA = 0;
  this->handleCommand.rPRB = 0;
  this->handleCommand.rPRC = 0;

  // Half speed.
  this->handleCommand.rSPA = 127;
  this->handleCommand.rSPB = 127;
  this->handleCommand.rSPC = 127;
}

////////////////////////////////////////////////////////////////////////////////
void VigirRobotiqHandPlugin::StopHand()
{
  // Set the target positions to the current ones.
  this->handleCommand.rPRA = this->handleState.gPRA;
  this->handleCommand.rPRB = this->handleState.gPRB;
  this->handleCommand.rPRC = this->handleState.gPRC;
}

////////////////////////////////////////////////////////////////////////////////
bool VigirRobotiqHandPlugin::IsHandFullyOpen()
{
  bool fingersOpen = true;

  // The hand will be fully open when all the fingers are within 'tolerance'
  // from their lower limits.
  gazebo::math::Angle tolerance;
  tolerance.SetFromDegree(1.0);

  for (int i = 2; i < this->NumJoints; ++i)
  {
    fingersOpen = fingersOpen &&
      (this->joints[i]->GetAngle(0) <
       (this->joints[i]->GetLowerLimit(0) + tolerance));
  }

  return fingersOpen;
}

////////////////////////////////////////////////////////////////////////////////
void VigirRobotiqHandPlugin::UpdateStates()
{
  boost::mutex::scoped_lock lock(this->controlMutex);

  gazebo::common::Time curTime = this->world->GetSimTime();

  // Step 1: State transitions.
  if (curTime > this->lastControllerUpdateTime)
  {
    this->userHandleCommand = this->handleCommand;

    // Deactivate gripper.
    if (this->handleCommand.rACT == 0)
    {
      this->handState = Disabled;
    }
    // Emergency auto-release.
    else if (this->handleCommand.rATR == 1)
    {
      this->handState = Emergency;
    }
    // Individual Control of Scissor.
    else if (this->handleCommand.rICS == 1)
    {
      this->handState = ICS;
    }
    // Individual Control of Fingers.
    else if (this->handleCommand.rICF == 1)
    {
      this->handState = ICF;
    }
    else
    {
      // Change the grasping mode.
      if (static_cast<int>(this->handleCommand.rMOD) != this->graspingMode)
      {
        this->handState = ChangeModeInProgress;
        lastHandleCommand = handleCommand;

        // Update the grasping mode.
        this->graspingMode =
          static_cast<GraspingMode>(this->handleCommand.rMOD);
      }
      else if (this->handState != ChangeModeInProgress)
      {
        this->handState = Simplified;
      }

      // Grasping mode initialized, let's change the state to Simplified Mode.
      if (this->handState == ChangeModeInProgress && this->IsHandFullyOpen())
      {
        this->prevCommand = this->handleCommand;

        // Restore the original command.
        this->handleCommand = this->lastHandleCommand;
        this->handState = Simplified;
      }
    }

    // Step 2: Actions in each state.
    switch (this->handState)
    {
      case Disabled:
        break;

      case Emergency:
        // Open the hand.
        if (this->IsHandFullyOpen())
          this->StopHand();
        else
          this->ReleaseHand();
        break;

      case ICS:
        std::cerr << "Individual Control of Scissor not supported" << std::endl;
        break;

      case ICF:
        if (this->handleCommand.rGTO == 0)
        {
          // "Stop" action.
          this->StopHand();
        }
        break;

      case ChangeModeInProgress:
        // Open the hand.
        this->ReleaseHand();
        break;

      case Simplified:
        // We are in Simplified mode, so all the fingers should follow finger A.
        // Position.
        this->handleCommand.rPRB = this->handleCommand.rPRA;
        this->handleCommand.rPRC = this->handleCommand.rPRA;
        // Velocity.
        this->handleCommand.rSPB = this->handleCommand.rSPA;
        this->handleCommand.rSPC = this->handleCommand.rSPA;
        // Force.
        this->handleCommand.rFRB = this->handleCommand.rFRA;
        this->handleCommand.rFRC = this->handleCommand.rFRA;

        if (this->handleCommand.rGTO == 0)
        {
          // "Stop" action.
          this->StopHand();
        }
        break;

      default:
        std::cerr << "Unrecognized state [" << this->handState << "]"
                  << std::endl;
    }

    // Update the hand controller.
    this->UpdatePIDControl((curTime - this->lastControllerUpdateTime).Double());

    // Gather robot state data and publish them.
    this->GetAndPublishHandleState();

    // Publish joint states.
    this->GetAndPublishJointState(curTime);

    this->lastControllerUpdateTime = curTime;
  }
}

////////////////////////////////////////////////////////////////////////////////
uint8_t VigirRobotiqHandPlugin::GetObjectDetection(
  const gazebo::physics::JointPtr &_joint, int _index, uint8_t _rPR,
  uint8_t _prevrPR)
{
  // Check finger's speed.
  bool isMoving = _joint->GetVelocity(0) > this->VelTolerance;

  // Check if the finger reached its target positions. We look at the error in
  // the position PID to decide if reached the target.
  double pe, ie, de;
  this->posePID[_index].GetErrors(pe, ie, de);
  bool reachPosition = pe < this->PoseTolerance;

  if (isMoving)
  {
    // Finger is in motion.
    return 0;
  }
  else
  {
    if (reachPosition)
    {
      // Finger is at the requestedPosition.
      return 3;
    }
    else if (_rPR - _prevrPR > 0)
    {
      // Finger has stopped due to a contact while closing.
      return 2;
    }
    else
    {
      // Finger has stopped due to a contact while opening.
      return 1;
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
uint8_t VigirRobotiqHandPlugin::GetCurrentPosition(
  const gazebo::physics::JointPtr &_joint)
{
  // Full range of motion.
  gazebo::math::Angle range =
    _joint->GetUpperLimit(0) - _joint->GetLowerLimit(0);

  // The maximum value in pinch mode is 177.
  if (this->graspingMode == Pinch)
    range *= 177.0 / 255.0;

  // Angle relative to the lower limit.
  gazebo::math::Angle relAngle = _joint->GetAngle(0) - _joint->GetLowerLimit(0);

  return
    static_cast<uint8_t>(round(255.0 * relAngle.Radian() / range.Radian()));
}

////////////////////////////////////////////////////////////////////////////////
void VigirRobotiqHandPlugin::GetAndPublishHandleState()
{
  // gACT. Initialization status.
  this->handleState.gACT = this->userHandleCommand.rACT;

  // gMOD. Operation mode status.
  this->handleState.gMOD = this->userHandleCommand.rMOD;

  // gGTO. Action status.
  this->handleState.gGTO = this->userHandleCommand.rGTO;

  // gIMC. Gripper status.
  if (this->handState == Emergency)
    this->handleState.gIMC = 0;
  else if (this->handState == ChangeModeInProgress)
    this->handleState.gIMC = 2;
  else
    this->handleState.gIMC = 3;

  // Check fingers' speed.
  bool isMovingA = this->joints[2]->GetVelocity(0) > this->VelTolerance;
  bool isMovingB = this->joints[3]->GetVelocity(0) > this->VelTolerance;
  bool isMovingC = this->joints[4]->GetVelocity(0) > this->VelTolerance;

  // Check if the fingers reached their target positions.
  double pe, ie, de;
  this->posePID[2].GetErrors(pe, ie, de);
  bool reachPositionA = pe < this->PoseTolerance;
  this->posePID[3].GetErrors(pe, ie, de);
  bool reachPositionB = pe < this->PoseTolerance;
  this->posePID[4].GetErrors(pe, ie, de);
  bool reachPositionC = pe < this->PoseTolerance;

  // gSTA. Motion status.
  if (isMovingA || isMovingB || isMovingC)
  {
    // Gripper is in motion.
    this->handleState.gSTA = 0;
  }
  else
  {
    if (reachPositionA && reachPositionB && reachPositionC)
    {
      // Gripper is stopped: All fingers reached requested position.
      this->handleState.gSTA = 3;
    }
    else if (!reachPositionA && !reachPositionB && !reachPositionC)
    {
      // Gripper is stopped: All fingers stopped before requested position.
      this->handleState.gSTA = 2;
    }
    else
    {
      // Gripper stopped. One or two fingers stopped before requested position.
      this->handleState.gSTA = 1;
    }
  }

  // gDTA. Finger A object detection.
  this->handleState.gDTA = this->GetObjectDetection(this->joints[2], 2,
    this->handleCommand.rPRA, this->prevCommand.rPRA);

  // gDTB. Finger B object detection.
  this->handleState.gDTB = this->GetObjectDetection(this->joints[3], 3,
    this->handleCommand.rPRB, this->prevCommand.rPRB);

  // gDTC. Finger C object detection
  this->handleState.gDTC = this->GetObjectDetection(this->joints[4], 4,
    this->handleCommand.rPRC, this->prevCommand.rPRC);

  // gDTS. Scissor object detection. We use finger A as a reference.
  this->handleState.gDTS = this->GetObjectDetection(this->joints[0], 0,
    this->handleCommand.rPRS, this->prevCommand.rPRS);

  // gFLT. Fault status.
  if (this->handState == ChangeModeInProgress)
    this->handleState.gFLT = 6;
  else if (this->handState == Disabled)
    this->handleState.gFLT = 7;
  else if (this->handState == Emergency)
    this->handleState.gFLT = 11;
  else
    this->handleState.gFLT = 0;

  // gPRA. Echo of requested position for finger A.
  this->handleState.gPRA = this->userHandleCommand.rPRA;
  // gPOA. Finger A position [0-255].
  this->handleState.gPOA = this->GetCurrentPosition(this->joints[2]);
  // gCUA. Not implemented.
  this->handleState.gCUA = 0;

  // gPRB. Echo of requested position for finger B.
  this->handleState.gPRB = this->userHandleCommand.rPRB;
  // gPOB. Finger B position [0-255].
  this->handleState.gPOB = this->GetCurrentPosition(this->joints[3]);
  // gCUB. Not implemented.
  this->handleState.gCUB = 0;

  // gPRC. Echo of requested position for finger C.
  this->handleState.gPRC = this->userHandleCommand.rPRC;
  // gPOC. Finger C position [0-255].
  this->handleState.gPOC = this->GetCurrentPosition(this->joints[4]);
  // gCUS. Not implemented.
  this->handleState.gCUC = 0;

  // gPRS. Echo of requested position of the scissor action
  this->handleState.gPRS = this->userHandleCommand.rPRS;
  // gPOS. Scissor current position [0-255]. We use finger B as reference.
  this->handleState.gPOS = this->GetCurrentPosition(this->joints[1]);
  // gCUS. Not implemented.
  this->handleState.gCUS = 0;

  // Publish robot states.
  this->pubHandleStateQueue->push(this->handleState, this->pubHandleState);
}

////////////////////////////////////////////////////////////////////////////////
void VigirRobotiqHandPlugin::GetAndPublishJointState(
                                           const gazebo::common::Time &_curTime)
{
  this->jointStates.header.stamp = ros::Time(_curTime.sec, _curTime.nsec);
  for (size_t i = 0; i < this->joints.size(); ++i)
  {
    this->jointStates.position[i] = this->joints[i]->GetAngle(0).Radian();
    this->jointStates.velocity[i] = this->joints[i]->GetVelocity(0);
    // better to use GetForceTorque dot joint axis
    this->jointStates.effort[i] = this->joints[i]->GetForce(0u);
  }
  this->pubJointStatesQueue->push(this->jointStates, this->pubJointStates);
}

////////////////////////////////////////////////////////////////////////////////
void VigirRobotiqHandPlugin::UpdatePIDControl(double _dt)
{
  if (this->handState == Disabled)
  {
    for (int i = 0; i < this->NumJoints; ++i)
      this->fingerJoints[i]->SetForce(0, 0.0);

    return;
  }

  for (int i = 0; i < this->NumJoints; ++i)
  {
    double targetPose = 0.0;
    double targetSpeed = (this->MinVelocity + this->MaxVelocity) / 2.0;

    if (i == 0)
    {
      switch (this->graspingMode)
      {
        case Wide:
          targetPose = this->joints[i]->GetUpperLimit(0).Radian();
          break;

        case Pinch:
          // --11 degrees.
          targetPose = -0.1919;
          break;

        case Scissor:
          // Max position is reached at value 215.
          targetPose = this->joints[i]->GetUpperLimit(0).Radian() -
            (this->joints[i]->GetUpperLimit(0).Radian() -
             this->joints[i]->GetLowerLimit(0).Radian()) * (215.0 / 255.0)
            * this->handleCommand.rPRA / 255.0;
          break;
      }
    }
    else if (i == 1)
    {
      switch (this->graspingMode)
      {
        case Wide:
          targetPose = this->joints[i]->GetLowerLimit(0).Radian();
          break;

        case Pinch:
          // 11 degrees.
          targetPose = 0.1919;
          break;

        case Scissor:
        // Max position is reached at value 215.
          targetPose = this->joints[i]->GetLowerLimit(0).Radian() +
            (this->joints[i]->GetUpperLimit(0).Radian() -
             this->joints[i]->GetLowerLimit(0).Radian()) * (215.0 / 255.0)
            * this->handleCommand.rPRA / 255.0;
          break;
      }
    }
    else if (i >= 2 && i <= 4)
    {
      if (this->graspingMode == Pinch)
      {
        // Max position is reached at value 177.
        targetPose = this->joints[i]->GetLowerLimit(0).Radian() +
          (this->joints[i]->GetUpperLimit(0).Radian() -
           this->joints[i]->GetLowerLimit(0).Radian()) * (177.0 / 255.0)
          * this->handleCommand.rPRA / 255.0;
      }
      else if (this->graspingMode == Scissor)
      {
        targetSpeed = this->MinVelocity +
          ((this->MaxVelocity - this->MinVelocity) *
          this->handleCommand.rSPA / 255.0);
      }
      else
      {
        targetPose = this->joints[i]->GetLowerLimit(0).Radian() +
          (this->joints[i]->GetUpperLimit(0).Radian() -
           this->joints[i]->GetLowerLimit(0).Radian())
          * this->handleCommand.rPRA / 255.0;
      }
    }

    // Get the current pose.
    double currentPose = this->joints[i]->GetAngle(0).Radian();

    // Position error.
    double poseError = currentPose - targetPose;

    // Update the PID.
    double torque = this->posePID[i].Update(poseError, _dt);

    // Apply the PID command.
    this->fingerJoints[i]->SetForce(0, torque);
  }
}

////////////////////////////////////////////////////////////////////////////////
bool VigirRobotiqHandPlugin::GetAndPushBackJoint(const std::string& _jointName,
                                            gazebo::physics::Joint_V& _joints)
{
  gazebo::physics::JointPtr joint = this->model->GetJoint(_jointName);

  if (!joint)
  {
    gzerr << "Failed to find joint [" << _jointName
          << "] aborting plugin load." << std::endl;
    return false;
  }
  _joints.push_back(joint);
  gzlog << "VigirRobotiqHandPlugin found joint [" << _jointName << "]" << std::endl;
  return true;
}

////////////////////////////////////////////////////////////////////////////////
bool VigirRobotiqHandPlugin::FindJoints()
{
  // Load up the joints we expect to use, finger by finger.
  gazebo::physics::JointPtr joint;
  std::string prefix;
  std::string suffix;
  if (this->side == "left")
    prefix = "left_";
  else
    prefix = "right_";

  // palm_finger_1_joint (actuated).
  suffix = "f2_j0";
  if (!this->GetAndPushBackJoint(prefix + suffix, this->joints))
    return false;
  if (!this->GetAndPushBackJoint(prefix + suffix, this->fingerJoints))
    return false;
  this->jointNames.push_back(prefix + suffix);

  // palm_finger_2_joint (actuated).
  suffix = "f1_j0";
  if (!this->GetAndPushBackJoint(prefix + suffix, this->joints))
    return false;
  if (!this->GetAndPushBackJoint(prefix + suffix, this->fingerJoints))
    return false;
  this->jointNames.push_back(prefix + suffix);

  // We read the joint state from finger_1_joint_1
  // but we actuate finger_1_joint_proximal_actuating_hinge (actuated).
  suffix = "f2_j1";
  if (!this->GetAndPushBackJoint(prefix + suffix, this->fingerJoints))
    return false;
  if (!this->GetAndPushBackJoint(prefix + suffix, this->joints))
    return false;
  this->jointNames.push_back(prefix + suffix);

  // We read the joint state from finger_2_joint_1
  // but we actuate finger_2_proximal_actuating_hinge (actuated).
  suffix = "f1_j1";
  if (!this->GetAndPushBackJoint(prefix + suffix, this->fingerJoints))
    return false;
  if (!this->GetAndPushBackJoint(prefix + suffix, this->joints))
    return false;
  this->jointNames.push_back(prefix + suffix);

  // We read the joint state from finger_middle_joint_1
  // but we actuate finger_middle_proximal_actuating_hinge (actuated).
  suffix = "f0_j1";
  if (!this->GetAndPushBackJoint(prefix + suffix, this->fingerJoints))
    return false;
  if (!this->GetAndPushBackJoint(prefix + suffix, this->joints))
    return false;
  this->jointNames.push_back(prefix + suffix);

  // finger_1_joint_2 (underactuated).
  suffix = "f2_j2";
  if (!this->GetAndPushBackJoint(prefix + suffix, this->joints))
    return false;
  if (!this->GetAndPushBackJoint(prefix + suffix, this->fingerJoints))
    return false;
  this->jointNames.push_back(prefix + suffix);

  // finger_1_joint_3 (underactuated).
  suffix = "f2_j3";
  if (!this->GetAndPushBackJoint(prefix + suffix, this->joints))
    return false;
  if (!this->GetAndPushBackJoint(prefix + suffix, this->fingerJoints))
    return false;
  this->jointNames.push_back(prefix + suffix);

  // finger_2_joint_2 (underactuated).
  suffix = "f1_j2";
  if (!this->GetAndPushBackJoint(prefix + suffix, this->joints))
    return false;
  if (!this->GetAndPushBackJoint(prefix + suffix, this->fingerJoints))
    return false;
  this->jointNames.push_back(prefix + suffix);

  // finger_2_joint_3 (underactuated).
  suffix = "f1_j3";
  if (!this->GetAndPushBackJoint(prefix + suffix, this->joints))
    return false;
  if (!this->GetAndPushBackJoint(prefix + suffix, this->fingerJoints))
    return false;
  this->jointNames.push_back(prefix + suffix);

  // finger_middle_joint_2 (underactuated).
  suffix = "f0_j2";
  if (!this->GetAndPushBackJoint(prefix + suffix, this->joints))
    return false;
  if (!this->GetAndPushBackJoint(prefix + suffix, this->fingerJoints))
    return false;
  this->jointNames.push_back(prefix + suffix);

  // finger_middle_joint_3 (underactuated).
  suffix = "f0_j3";
  if (!this->GetAndPushBackJoint(prefix + suffix, this->joints))
    return false;
  if (!this->GetAndPushBackJoint(prefix + suffix, this->fingerJoints))
    return false;
  this->jointNames.push_back(prefix + suffix);

  gzlog << "VigirRobotiqHandPlugin found all joints for " << this->side
        << " hand." << std::endl;
  return true;
}

////////////////////////////////////////////////////////////////////////////////
void VigirRobotiqHandPlugin::RosQueueThread()
{
  static const double timeout = 0.01;

  while (this->rosNode->ok())
  {
    this->rosQueue.callAvailable(ros::WallDuration(timeout));
  }
}

GZ_REGISTER_MODEL_PLUGIN(VigirRobotiqHandPlugin)
