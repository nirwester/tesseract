﻿
#include <tesseract_common/macros.h>
TESSERACT_COMMON_IGNORE_WARNINGS_PUSH
#include <functional>
#include <taskflow/taskflow.hpp>
#include <fstream>
TESSERACT_COMMON_IGNORE_WARNINGS_POP
#include <tesseract_motion_planners/trajopt/profile/trajopt_default_plan_profile.h>
#include <tesseract_motion_planners/trajopt/profile/trajopt_default_composite_profile.h>
#include <tesseract_motion_planners/trajopt/problem_generators/default_problem_generator.h>

#include <tesseract_process_managers/process_generators/random_process_generator.h>
#include <tesseract_process_managers/process_generators/trajopt_process_generator.h>
#include <tesseract_process_managers/taskflow_generators/sequential_failure_tree_taskflow.h>
#include <tesseract_process_managers/process_managers/default_processes/default_freespace_processes.h>
#include <tesseract_process_managers/process_managers/default_processes/default_raster_processes.h>
#include <tesseract_process_managers/process_managers/raster_process_manager.h>
#include <tesseract_command_language/command_language_utils.h>

using namespace tesseract_planning;

RasterProcessManager::RasterProcessManager() : taskflow("RasterProcessManagerTaskflow") {}

/**
 * Composite
 * {
 *   Composite - from start
 *   Composite - Raster
 *   {
 *     Composite
 *       ...
 *     Composite
 *   }
 *   Unordered Composite - Transitions
 *   {
 *     Composite
 *       ...
 *     Composite
 *   }
 *   Composite - Raster
 *   {
 *     Composite
 *       ...
 *     Composite
 *   }
 *   Composite - to end
 * }
 */

bool RasterProcessManager::init(ProcessInput input)
{
  // This should make all of the isComposite checks so that you can safely cast below
  if (!checkProcessInput(input))
  {
    CONSOLE_BRIDGE_logError("Invalid Process Input");
    return false;
  }

  // Clear the taskflow
  taskflow.clear();

  // If no processes selected, use defaults
  if (freespace_process_generators.empty())
    freespace_process_generators = defaultFreespaceProcesses();
  if (raster_process_generators.empty())
    raster_process_generators = defaultRasterProcesses();

  // Create the taskflow generators
  freespace_taskflow_generator = SequentialFailureTreeTaskflow(freespace_process_generators);
  raster_taskflow_generator = SequentialFailureTreeTaskflow(raster_process_generators);

  // Store the current size of the tasks so that we can add from_start later
  std::size_t starting_raster_idx = raster_tasks.size();

  // Generate all of the raster tasks. They don't depend on anything
  for (std::size_t idx = 1; idx < input.size() - 1; idx += 2)
  {
    // Rasters can have multiple steps (e.g. approach, process, departure), but they are all flattened
    auto raster_step = taskflow
                           .composed_of(raster_taskflow_generator.generateTaskflow(
                               input[idx],
                               std::bind(&RasterProcessManager::successCallback, this),
                               std::bind(&RasterProcessManager::failureCallback, this)))
                           .name("raster_" + std::to_string(idx));
    raster_tasks.push_back(raster_step);
  }

  // Loop over all transitions
  std::size_t transition_idx = 0;
  for (std::size_t input_idx = 2; input_idx < input.size() - 1; input_idx += 2)
  {
    // Each transition step depends on the start and end only since they are independent
    for (std::size_t transition_step_idx = 0; transition_step_idx < input[input_idx].size(); transition_step_idx++)
    {
      auto transition_step =
          taskflow
              .composed_of(freespace_taskflow_generator.generateTaskflow(
                  input[input_idx][transition_step_idx],
                  std::bind(&RasterProcessManager::successCallback, this),
                  std::bind(&RasterProcessManager::failureCallback, this)))
              .name("transition_" + std::to_string(input_idx) + "." + std::to_string(transition_step_idx));

      // Each transition is independent and thus depends only on the adjacent rasters
      transition_step.succeed(raster_tasks[starting_raster_idx + transition_idx]);
      transition_step.succeed(raster_tasks[starting_raster_idx + transition_idx + 1]);
      freespace_tasks.push_back(transition_step);
    }
    transition_idx++;
  }

  // Plan from_start - preceded by the first raster
  auto from_start = taskflow
                        .composed_of(freespace_taskflow_generator.generateTaskflow(
                            input[0],
                            std::bind(&RasterProcessManager::successCallback, this),
                            std::bind(&RasterProcessManager::failureCallback, this)))
                        .name("from_start");
  raster_tasks[starting_raster_idx].precede(from_start);
  freespace_tasks.push_back(from_start);

  // Plan from_start - preceded by the last raster
  auto to_end = taskflow
                    .composed_of(freespace_taskflow_generator.generateTaskflow(
                        input[input.size() - 1],
                        std::bind(&RasterProcessManager::successCallback, this),
                        std::bind(&RasterProcessManager::failureCallback, this)))
                    .name("to_end");
  raster_tasks.back().precede(to_end);
  freespace_tasks.push_back(to_end);

  // visualizes the taskflow
  std::ofstream out_data;
  out_data.open("raster_process_manager.dot");
  taskflow.dump(out_data);
  out_data.close();

  return true;
}

bool RasterProcessManager::execute()
{
  success = false;
  executor.run(taskflow).wait();
  return success;
}

bool RasterProcessManager::terminate()
{
  CONSOLE_BRIDGE_logError("Terminate is not implemented");
  return false;
}

bool RasterProcessManager::clear()

{
  taskflow.clear();
  //  freespace_tasks.clear();
  return true;
}

bool RasterProcessManager::checkProcessInput(const tesseract_planning::ProcessInput& input) const
{
  // -------------
  // Check Input
  // -------------
  if (!input.tesseract)
  {
    CONSOLE_BRIDGE_logError("ProcessInput tesseract is a nullptr");
    return false;
  }

  // Check the overall input
  if (!isCompositeInstruction(input.instruction))
  {
    CONSOLE_BRIDGE_logError("ProcessInput Invalid: input.instructions should be a composite");
    return false;
  }
  auto composite = *input.instruction.cast_const<CompositeInstruction>();

  // Check from_start
  if (!isCompositeInstruction(composite.at(0)))
  {
    CONSOLE_BRIDGE_logError("ProcessInput Invalid: from_start should be a composite");
    return false;
  }

  // Check rasters and transitions
  for (std::size_t index = 1; index < composite.size() - 1; index++)
  {
    // Both rasters and transitions should be a composite
    if (!isCompositeInstruction(composite[index]))
    {
      CONSOLE_BRIDGE_logError("ProcessInput Invalid: Both rasters and transitions should be a composite");
      return false;
    }

    // Convert to composite
    auto step = *composite[index].cast_const<CompositeInstruction>();

    // Odd numbers are raster segments
    if (index % 2 == 1)
    {
      // Raster must have at least one element but 3 is not enforced.
      if (!step.size())
      {
        CONSOLE_BRIDGE_logError("ProcessInput Invalid: Rasters must have at least one element");
        return false;
      }
      //      for (const auto& raster_step : step)
      //      {
      // TODO: Disabling this for now since we are flattening it. I'm not sure if this should be a requirement or not
      //        // However, all steps of the raster must be composites
      //        if (!isCompositeInstruction(raster_step))
      //        {
      //          CONSOLE_BRIDGE_logError("ProcessInput Invalid: All steps of a raster should be a composite");
      //          return false;
      //        }
      //      }
    }
    // Evens are transitions
    else
    {
      // If there is only one transition, we assume it is transition_from_end
      if (step.size() > 1)
      {
        // If there are multiple, then they should be unordered
        if (step.getOrder() != CompositeInstructionOrder::UNORDERED)
        {
          // If you get this error, check that this is not processing a raster strip. You may be missing from_start.
          CONSOLE_BRIDGE_logError("Raster contains multiple transitions but is not marked UNORDERED");
          step.print();
          return false;
        }
      }
    }
  }
  // Check to_end
  if (!isCompositeInstruction(composite.back()))
  {
    CONSOLE_BRIDGE_logError("ProcessInput Invalid: to_end should be a composite");
    return false;
  };

  return true;
}

void RasterProcessManager::successCallback()
{
  CONSOLE_BRIDGE_logInform("RasterProcessManager Successful");
  success = true;
}

void RasterProcessManager::failureCallback()
{
  CONSOLE_BRIDGE_logInform("RasterProcessManager Failure");
  success = false;
}