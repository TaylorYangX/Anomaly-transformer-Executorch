/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <executorch/backends/vulkan/runtime/graph/ops/OperatorRegistry.h>

#include <executorch/backends/vulkan/runtime/graph/ops/impl/Staging.h>

#include <executorch/backends/vulkan/runtime/graph/ops/impl/utils/ScalarUtils.h>
#include <executorch/backends/vulkan/runtime/graph/ops/impl/utils/TensorUtils.h>

#include <executorch/backends/vulkan/runtime/graph/ops/utils/ShaderNameUtils.h>

namespace vkcompute {

void check_matmul_args(
    const ComputeGraph& graph,
    const ValueRef mat1,
    const ValueRef mat2_data,
    const ValueRef out) {
  std::vector<int64_t> mat1_sizes = graph.sizes_of(mat1);
  std::vector<int64_t> mat2_sizes = graph.sizes_of(mat2_data);

  VK_CHECK_COND(mat1_sizes.size() == 2 || mat1_sizes.size() == 3);
  VK_CHECK_COND(mat1_sizes.size() == mat2_sizes.size());

  VK_CHECK_COND(graph.memory_layout_of(mat1) == graph.memory_layout_of(out));

  VK_CHECK_COND(
      api::utils::val_at(-1, mat1_sizes) == api::utils::val_at(-2, mat2_sizes));
}

void resize_matmul_node(
    ComputeGraph* graph,
    const std::vector<ArgGroup>& args,
    const std::vector<ValueRef>& extra_args) {
  (void)extra_args;
  vTensorPtr out = graph->get_tensor(args[0].refs[0]);
  vTensorPtr mat1 = graph->get_tensor(args[1].refs[0]);
  vTensorPtr mat2 = graph->get_tensor(args[1].refs[1]);

  std::vector<int64_t> new_out_sizes(3);
  if (mat1->sizes().size() == 2) {
    new_out_sizes.resize(2);
    new_out_sizes.at(0) = mat1->sizes().at(0);
    new_out_sizes.at(1) = mat2->sizes().at(1);
  } else {
    new_out_sizes.at(0) = mat1->sizes().at(0);
    new_out_sizes.at(1) = mat1->sizes().at(1);
    new_out_sizes.at(2) = mat2->sizes().at(2);
  }

  out->virtual_resize(new_out_sizes);
}

void add_matmul_naive_node(
    ComputeGraph& graph,
    const ValueRef mat1,
    const ValueRef mat2_data,
    const ValueRef out) {
  ValueRef mat2 = prepack_if_tensor_ref(graph, mat2_data, api::kHeightPacked);

  api::utils::uvec3 global_size = graph.extents_of(out);
  api::utils::uvec3 local_size = adaptive_work_group_size(global_size);

  std::string kernel_name("matmul_naive");
  kernel_name.reserve(kShaderNameReserve);
  add_memory_layout_suffix(kernel_name, graph.memory_layout_of(mat1));
  add_memory_layout_suffix(kernel_name, graph.memory_layout_of(mat2));
  add_dtype_suffix(kernel_name, graph.dtype_of(out));

  graph.execute_nodes().emplace_back(new ExecuteNode(
      graph,
      VK_KERNEL_FROM_STR(kernel_name),
      global_size,
      local_size,
      // Inputs and Outputs
      {{out, api::MemoryAccessType::WRITE},
       {{mat1, mat2}, api::MemoryAccessType::READ}},
      // Shader params buffers
      {
          graph.texture_limits_ubo(out),
          graph.sizes_ubo(mat1),
      },
      // Specialization Constants
      {},
      // Resizing Logic
      resize_matmul_node));
}

void add_matmul_optimized_node(
    ComputeGraph& graph,
    const ValueRef mat1,
    const ValueRef mat2_data,
    const ValueRef out) {
  ValueRef mat2 = prepack_if_tensor_ref(graph, mat2_data, api::kHeightPacked);

  // Ensure mat1 is width packed
  ValueRef mat1_W_packed = graph.add_tensor_like(mat1, api::kWidthPacked);
  auto viewFn = VK_GET_OP_FN("aten.view_copy.default");
  viewFn(graph, {mat1, graph.add_none(), mat1_W_packed});

  // Ensure mat2 to height packed
  ValueRef mat2_H_packed = mat2;
  if (graph.memory_layout_of(mat2) != api::kHeightPacked) {
    mat2_H_packed = graph.add_tensor_like(mat2, api::kHeightPacked);
    viewFn(graph, {mat2, graph.add_none(), mat2_H_packed});
  }

  api::utils::uvec3 global_size =
      api::utils::divup_vec(graph.extents_of(out), {4, 4, 1});
  api::utils::uvec3 local_size = adaptive_work_group_size(global_size);

  std::string kernel_name("matmul_optimized");
  add_dtype_suffix(kernel_name, graph.dtype_of(out));

  graph.execute_nodes().emplace_back(new ExecuteNode(
      graph,
      VK_KERNEL_FROM_STR(kernel_name),
      global_size,
      local_size,
      // Inputs and Outputs
      {{out, api::MemoryAccessType::WRITE},
       {{mat1_W_packed, mat2_H_packed}, api::MemoryAccessType::READ}},
      // Shader params buffers
      {
          graph.texture_limits_ubo(out),
          graph.sizes_ubo(out),
          graph.packed_dim_meta_ubo(mat1_W_packed),
      },
      // Specialization Constants
      {}));
}

void add_matmul_node(
    ComputeGraph& graph,
    const ValueRef mat1,
    const ValueRef mat2_data,
    const ValueRef out) {
  if (graph.memory_layout_of(mat1) == api::kChannelsPacked) {
    add_matmul_optimized_node(graph, mat1, mat2_data, out);
  } else if (graph.memory_layout_of(mat1) == api::kWidthPacked) {
    add_matmul_naive_node(graph, mat1, mat2_data, out);
  } else {
    VK_THROW("Input should be channel packed or width packed.");
  }
}

void matmul(ComputeGraph& graph, const std::vector<ValueRef>& args) {
  check_matmul_args(graph, args[0], args[1], args[2]);
  return add_matmul_node(graph, args[0], args[1], args[2]);
}

REGISTER_OPERATORS {
  VK_REGISTER_OP(aten.mm.default, matmul);
  VK_REGISTER_OP(aten.bmm.default, matmul);
}

} // namespace vkcompute
