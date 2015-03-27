#include <carl_moveit/common_actions.h>

using namespace std;

CommonActions::CommonActions() :
    moveToJointPoseClient("carl_moveit_wrapper/move_to_joint_pose"),
    liftServer(n, "carl_moveit_wrapper/common_actions/lift", boost::bind(&CommonActions::liftArm, this, _1), false),
    armServer(n, "carl_moveit_wrapper/common_actions/arm_action", boost::bind(&CommonActions::executeArmAction, this, _1), false)
{
  //setup home position
  homePosition.resize(NUM_JACO_JOINTS);
  homePosition[0] = -1.410;
  homePosition[1] = 2.975;
  homePosition[2] = .868;
  homePosition[3] = -2.323;
  homePosition[4] = 1.626;
  homePosition[5] = 1.393;

  defaultRetractPosition.resize(NUM_JACO_JOINTS);
  defaultRetractPosition[0] = -2.57;
  defaultRetractPosition[1] = 1.39;
  defaultRetractPosition[2] = .527;
  defaultRetractPosition[3] = -.084;
  defaultRetractPosition[4] = .515;
  defaultRetractPosition[5] = -1.745;

  angularCmdPublisher = n.advertise<wpi_jaco_msgs::AngularCommand>("jaco_arm/angular_cmd", 1);

  eraseTrajectoriesClient = n.serviceClient<std_srvs::Empty>("jaco_arm/erase_trajectories");
  cartesianPathClient = n.serviceClient<carl_moveit::CartesianPath>("carl_moveit_wrapper/cartesian_path");
  jacoPosClient = n.serviceClient<wpi_jaco_msgs::GetAngularPosition>("jaco_arm/get_angular_position");

  //start action server
  liftServer.start();
  armServer.start();
}

void CommonActions::executeArmAction(const carl_moveit::ArmGoalConstPtr &goal)
{
  carl_moveit::ArmResult result;

  if (goal->action == carl_moveit::ArmGoal::RETRACT)
  {
    if (isArmRetracted(defaultRetractPosition))
    {
      ROS_INFO("Arm is already retracted.");
      result.success = true;
      armServer.setSucceeded(result);
      return;
    }
  }

  carl_moveit::MoveToJointPoseGoal jointPoseGoal;

  vector<float> baseJointPoseGoal;
  baseJointPoseGoal.resize(homePosition.size());
  for (unsigned int i = 0; i < baseJointPoseGoal.size(); i ++)
  {
    baseJointPoseGoal[i] = homePosition[i];
  }
  jointPoseGoal.joints.resize(baseJointPoseGoal.size());
  for (unsigned int i = 0; i < jointPoseGoal.joints.size(); i ++)
  {
    jointPoseGoal.joints[i] = baseJointPoseGoal[i];
  }
  bool succeeded = false;
  int counter = 0;
  int attempts = MAX_HOME_ATTEMPTS;
  while (!succeeded && counter < attempts)
  {
    ROS_INFO("Ready arm attempt %d", counter);

    moveToJointPoseClient.sendGoal(jointPoseGoal);
    ROS_INFO("Moving arm to ready position...");
    while (!moveToJointPoseClient.getState().isDone())
    {
      if (armServer.isPreemptRequested() || !ros::ok())
      {
        ROS_INFO("Ready arm action preempted.");
        moveToJointPoseClient.cancelAllGoals();
        result.success = false;
        armServer.setPreempted(result);
        return;
      }
    }

    carl_moveit::MoveToJointPoseResultConstPtr readyResult = moveToJointPoseClient.getResult();

    succeeded = readyResult->success;
    counter ++;

    //slightly vary joint goal and retry planning
    if (!succeeded && counter < attempts)
    {
      ROS_INFO("Ready arm failed, resampling goal for another attempt...");
      for (unsigned int i = 0; i < jointPoseGoal.joints.size(); i ++)
      {
        jointPoseGoal.joints[i] = baseJointPoseGoal[i] + (rand() % 700 - 350) / 10000;  //vary by up to ~2 degrees
      }
    }
  }

  if (!succeeded)
  {
    ROS_INFO("Plan and move to ready position failed.");
    result.success = false;
    armServer.setSucceeded(result);
    return;
  }

  if (goal->action == carl_moveit::ArmGoal::RETRACT)
  {
    wpi_jaco_msgs::AngularCommand cmd;
    cmd.armCommand = true;
    cmd.fingerCommand = false;
    cmd.position = true;
    cmd.repeat = false;
    cmd.joints = defaultRetractPosition;
    angularCmdPublisher.publish(cmd);
    ros::Time startTime = ros::Time::now();
    ros::Rate loopRate(30);
    bool retracted = false;
    while (!retracted)
    {
      //check for preempt
      if (armServer.isPreemptRequested() || !ros::ok())
      {
        ROS_INFO("Ready arm action preempted.");
        std_srvs::Empty srv;
        if (!eraseTrajectoriesClient.call(srv))
        {
          ROS_INFO("Could not call erase trajectories service...");
        }
        result.success = false;
        armServer.setPreempted(result);
        return;
      }

      //check if arm is retracted
      retracted = isArmRetracted(defaultRetractPosition);

      //check for timeout
      ros::Time currentTime = ros::Time::now();
      if ((currentTime.toSec() - startTime.toSec()) > 10.0)
      {
        ROS_INFO("Ready arm timed out.");
        result.success = false;
        armServer.setSucceeded(result);
        return;
      }
      loopRate.sleep();
    }
  }

  ROS_INFO("Ready arm finished.");

  result.success = succeeded;
  armServer.setSucceeded(result);
}

void CommonActions::liftArm(const rail_manipulation_msgs::LiftGoalConstPtr &goal)
{
  rail_manipulation_msgs::LiftResult result;

  carl_moveit::CartesianPath srv;
  tf::StampedTransform currentHandTransform;
  tfListener.waitForTransform("jaco_link_hand", "base_footprint", ros::Time::now(), ros::Duration(1.0));
  tfListener.lookupTransform("base_footprint", "jaco_link_hand", ros::Time(0), currentHandTransform);
  geometry_msgs::Pose liftPose;
  liftPose.position.x = currentHandTransform.getOrigin().x();
  liftPose.position.y = currentHandTransform.getOrigin().y();
  liftPose.position.z = currentHandTransform.getOrigin().z() + .1;
  liftPose.orientation.x = currentHandTransform.getRotation().x();
  liftPose.orientation.y = currentHandTransform.getRotation().y();
  liftPose.orientation.z = currentHandTransform.getRotation().z();
  liftPose.orientation.w = currentHandTransform.getRotation().w();
  srv.request.waypoints.push_back(liftPose);
  srv.request.avoidCollisions = false;

  if (!cartesianPathClient.call(srv))
  {
    ROS_INFO("Could not call Jaco Cartesian path service.");
    result.success = false;
    liftServer.setAborted(result, "Could not call Jaco Cartesian path service.");
    return;
  }

  result.success = srv.response.success;
  liftServer.setSucceeded(result);
}

bool CommonActions::isArmRetracted(const vector<float> &retractPos)
{
  float dstFromRetract = 0;

  //get joint positions
  wpi_jaco_msgs::GetAngularPosition::Request req;
  wpi_jaco_msgs::GetAngularPosition::Response res;
  if(!jacoPosClient.call(req, res))
  {
    ROS_INFO("Could not call Jaco joint position service.");
    return false;
  }

  for (unsigned int i = 0; i < retractPos.size(); i ++)
  {
    dstFromRetract += fabs(retractPos[i] - res.pos[i]);
  }

  if (dstFromRetract > 0.175)
    return false;
  return true;
}

int main(int argc, char **argv)
{
  ros::init(argc, argv, "carl_moveit_common_actions");

  CommonActions ca;

  ros::spin();
}
