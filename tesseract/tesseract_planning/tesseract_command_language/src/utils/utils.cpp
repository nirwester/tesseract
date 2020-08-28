/**
 * @file utils.cpp
 * @brief
 *
 * @author Levi Armstrong
 * @date June 15, 2020
 * @version TODO
 * @bug No known bugs
 *
 * @copyright Copyright (c) 2020, Southwest Research Institute
 *
 * @par License
 * Software License Agreement (Apache License)
 * @par
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 * @par
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <tesseract_common/macros.h>
TESSERACT_COMMON_IGNORE_WARNINGS_PUSH
#include <algorithm>
#include <console_bridge/console.h>
TESSERACT_COMMON_IGNORE_WARNINGS_POP

#include <tesseract_command_language/utils/utils.h>
#include <tesseract_command_language/instruction_type.h>
#include <tesseract_command_language/joint_waypoint.h>
#include <tesseract_command_language/cartesian_waypoint.h>
#include <tesseract_command_language/state_waypoint.h>

namespace tesseract_planning
{
const Eigen::VectorXd& getJointPosition(const Waypoint& waypoint)
{
  if (isJointWaypoint(waypoint))
    return *(waypoint.cast_const<JointWaypoint>());

  if (isStateWaypoint(waypoint))
    return waypoint.cast_const<StateWaypoint>()->position;

  throw std::runtime_error("Unsupported waypoint type.");
}

bool setJointPosition(Waypoint& waypoint, const Eigen::Ref<const Eigen::VectorXd>& position)
{
  if (isJointWaypoint(waypoint))
    *waypoint.cast<JointWaypoint>() = position;
  else if (isStateWaypoint(waypoint))
    waypoint.cast<StateWaypoint>()->position = position;
  else
    return false;

  return true;
}

bool clampToJointLimits(Waypoint& wp, const Eigen::Ref<const Eigen::MatrixX2d>& limits)
{
  if (isJointWaypoint(wp) || isStateWaypoint(wp))
  {
    const Eigen::VectorXd cmd_pos = getJointPosition(wp);

    // Check input validity
    if (limits.rows() != cmd_pos.size())
    {
      CONSOLE_BRIDGE_logWarn(
          "Invalid limits when clamping Waypoint. Waypoint size: %d, Limits size: %d", limits.rows(), cmd_pos.size());
      return false;
    }

    // Does the position need adjusting?
    bool adjust_position = false;
    if ((limits.col(0).array() > cmd_pos.array()).any())
      adjust_position = true;
    if ((limits.col(1).array() < cmd_pos.array()).any())
      adjust_position = true;

    if (adjust_position)
    {
      CONSOLE_BRIDGE_logDebug("Clamping Waypoint to joint limits");
      Eigen::VectorXd new_position = cmd_pos;
      new_position = new_position.cwiseMax(limits.col(0));
      new_position = new_position.cwiseMin(limits.col(1));
      return setJointPosition(wp, new_position);
    }
  }

  return true;
}

void generateSkeletonSeedHelper(CompositeInstruction& composite_instructions)
{
  for (auto& i : composite_instructions)
  {
    if (isCompositeInstruction(i))
    {
      generateSkeletonSeedHelper(*(i.cast<CompositeInstruction>()));
    }
    else if (isPlanInstruction(i))
    {
      CompositeInstruction ci;
      const auto* pi = i.cast<PlanInstruction>();
      ci.setProfile(pi->getProfile());
      ci.setDescription(pi->getDescription());
      ci.setManipulatorInfo(pi->getManipulatorInfo());

      i = ci;
    }
  }
}

CompositeInstruction generateSkeletonSeed(const CompositeInstruction& composite_instructions)
{
  CompositeInstruction seed = composite_instructions;
  generateSkeletonSeedHelper(seed);
  return seed;
}

}  // namespace tesseract_planning
