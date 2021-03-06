// Copyright (c) 2015 Ionel Gog <ionel.gog@cl.cam.ac.uk>

/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * THIS CODE IS PROVIDED ON AN *AS IS* BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT
 * LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR
 * A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.
 *
 * See the Apache Version 2.0 License for specific language governing
 * permissions and limitations under the License.
 */

#include "ir/agg_operator_mpc.h"

#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace musketeer {
namespace ir {

  OperatorType AggOperatorMPC::get_type() {
    return AGG_OP_MPC;
  }

  bool AggOperatorMPC::isMPC() {
    return true;
  }

  OperatorInterface* AggOperatorMPC::clone() {
    return new AggOperatorMPC(get_input_dir(), get_condition_tree(), group_bys,
                              math_operator, get_relations(), columns,
                              get_output_relation());
  }

} // namespace ir
} // namespace musketeer
