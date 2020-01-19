#include "FN_multi_function_network.h"
#include "FN_multi_function_network_optimization.h"
#include "FN_multi_functions.h"

#include "BLI_stack_cxx.h"
#include "BLI_multi_map.h"
#include "BLI_rand.h"

namespace FN {

using BLI::MultiMap;
using BLI::Stack;

void optimize_network__remove_duplicates(MFNetworkBuilder &network_builder)
{
  Array<Optional<uint32_t>> hash_by_output_socket(network_builder.socket_id_amount());
  Array<bool> node_outputs_are_hashed(network_builder.node_id_amount(), false);

  RNG *rng = BLI_rng_new(0);

  for (MFBuilderDummyNode *node : network_builder.dummy_nodes()) {
    for (MFBuilderOutputSocket *output_socket : node->outputs()) {
      uint32_t output_hash = BLI_rng_get_uint(rng);
      hash_by_output_socket[output_socket->id()].set_new(output_hash);
    }
    node_outputs_are_hashed[node->id()] = true;
  }

  Stack<MFBuilderFunctionNode *> nodes_to_check = network_builder.function_nodes();
  while (!nodes_to_check.is_empty()) {
    MFBuilderFunctionNode &node = *nodes_to_check.peek();
    if (node_outputs_are_hashed[node.id()]) {
      nodes_to_check.pop();
      continue;
    }

    bool all_dependencies_ready = true;
    node.foreach_origin_node([&](MFBuilderNode &origin_node) {
      if (!node_outputs_are_hashed[origin_node.id()]) {
        all_dependencies_ready = false;
        nodes_to_check.push(&origin_node.as_function());
      }
    });

    if (!all_dependencies_ready) {
      continue;
    }

    uint32_t combined_inputs_hash = 827823743;
    for (MFBuilderInputSocket *input_socket : node.inputs()) {
      MFBuilderOutputSocket *origin = input_socket->origin();
      uint32_t input_hash;
      if (origin == nullptr) {
        input_hash = BLI_rng_get_uint(rng);
      }
      else {
        input_hash = *hash_by_output_socket[origin->id()];
      }

      combined_inputs_hash = combined_inputs_hash * 456123 + input_hash;
    }

    Optional<uint32_t> maybe_operation_hash = node.function().operation_hash();
    uint32_t operation_hash = (maybe_operation_hash.has_value()) ? *maybe_operation_hash :
                                                                   BLI_rng_get_uint(rng);
    uint32_t node_hash = combined_inputs_hash * 462347 + operation_hash;

    for (MFBuilderOutputSocket *output_socket : node.outputs()) {
      uint32_t output_hash = node_hash * (45234 + 567243 * output_socket->index());
      hash_by_output_socket[output_socket->id()].set_new(output_hash);
    }

    nodes_to_check.pop();
    node_outputs_are_hashed[node.id()] = true;
  }

  MultiMap<uint32_t, MFBuilderOutputSocket *> outputs_by_hash;
  for (uint id : hash_by_output_socket.index_range()) {
    Optional<uint32_t> maybe_hash = hash_by_output_socket[id];
    if (maybe_hash.has_value()) {
      uint32_t hash = *maybe_hash;
      MFBuilderOutputSocket &socket = network_builder.socket_by_id(id).as_output();
      outputs_by_hash.add(hash, &socket);
    }
  }

  outputs_by_hash.foreach_item(
      [&](uint32_t UNUSED(hash), ArrayRef<MFBuilderOutputSocket *> outputs_with_hash) {
        if (outputs_with_hash.size() <= 1) {
          return;
        }

        MFBuilderOutputSocket &deduplicated_output = *outputs_with_hash[0];
        for (MFBuilderOutputSocket *socket : outputs_with_hash.drop_front(1)) {
          BLI::ScopedVector<MFBuilderInputSocket *> targets_copy = socket->targets();
          for (MFBuilderInputSocket *target : targets_copy) {
            network_builder.relink_origin(deduplicated_output, *target);
          }
        }
      });

  BLI_rng_free(rng);
}

void optimize_network__remove_unused_nodes(MFNetworkBuilder &network_builder)
{
  ArrayRef<MFBuilderNode *> dummy_nodes = network_builder.dummy_nodes();
  Vector<MFBuilderNode *> nodes = network_builder.find_nodes_not_to_the_left_of__exclusive__vector(
      dummy_nodes);
  network_builder.remove_nodes(nodes);
}

void optimize_network__constant_folding(MFNetworkBuilder &network_builder,
                                        ResourceCollector &resources)
{
  Vector<MFBuilderNode *> non_constant_nodes;
  non_constant_nodes.extend(network_builder.dummy_nodes());
  for (MFBuilderFunctionNode *node : network_builder.function_nodes()) {
    if (node->function().depends_on_context()) {
      non_constant_nodes.append(node);
    }
  }

  Array<bool> node_is_not_constant = network_builder.find_nodes_to_the_right_of__inclusive__mask(
      non_constant_nodes);
  Vector<MFBuilderNode *> constant_builder_nodes = network_builder.nodes_by_id_inverted_id_mask(
      node_is_not_constant);
  // network_builder.to_dot__clipboard(constant_builder_nodes.as_ref());

  Vector<MFBuilderDummyNode *> dummy_nodes_to_compute;
  for (MFBuilderNode *node : constant_builder_nodes) {
    if (node->inputs().size() == 0) {
      continue;
    }

    for (MFBuilderOutputSocket *output_socket : node->outputs()) {
      MFDataType data_type = output_socket->data_type();

      for (MFBuilderInputSocket *target_socket : output_socket->targets()) {
        MFBuilderNode &target_node = target_socket->node();
        if (!node_is_not_constant[target_node.id()]) {
          continue;
        }

        MFBuilderDummyNode &dummy_node = network_builder.add_dummy(
            "Dummy", {data_type}, {}, {"Value"}, {});
        network_builder.add_link(*output_socket, dummy_node.input(0));
        dummy_nodes_to_compute.append(&dummy_node);
        break;
      }
    }
  }

  if (dummy_nodes_to_compute.size() == 0) {
    return;
  }

  MFNetwork network{network_builder};

  Vector<const MFInputSocket *> sockets_to_compute;
  for (MFBuilderDummyNode *dummy_node : dummy_nodes_to_compute) {
    uint node_index = network_builder.current_index_of(*dummy_node);
    sockets_to_compute.append(&network.dummy_nodes()[node_index]->input(0));
  }

  MF_EvaluateNetwork network_function{{}, sockets_to_compute};

  MFContextBuilder context_builder;
  MFParamsBuilder params_builder{network_function, 1};

  for (uint param_index : network_function.param_indices()) {
    MFParamType param_type = network_function.param_type(param_index);
    BLI_assert(param_type.is_output());
    MFDataType data_type = param_type.data_type();

    switch (data_type.category()) {
      case MFDataType::Single: {
        const CPPType &cpp_type = data_type.single__cpp_type();
        void *buffer = resources.allocate(cpp_type.size(), cpp_type.alignment());
        GenericMutableArrayRef array{cpp_type, buffer, 1};
        params_builder.add_single_output(array);
        break;
      }
      case MFDataType::Vector: {
        const CPPType &cpp_base_type = data_type.vector__cpp_base_type();
        GenericVectorArray &vector_array = resources.construct<GenericVectorArray>(
            "constant vector", cpp_base_type, 1);
        params_builder.add_vector_output(vector_array);
        break;
      }
    }
  }

  network_function.call(IndexRange(1), params_builder, context_builder);

  for (uint param_index : network_function.param_indices()) {
    MFParamType param_type = network_function.param_type(param_index);
    MFDataType data_type = param_type.data_type();

    const MultiFunction *constant_fn = nullptr;

    switch (data_type.category()) {
      case MFDataType::Single: {
        const CPPType &cpp_type = data_type.single__cpp_type();

        GenericMutableArrayRef array = params_builder.computed_array(param_index);
        void *buffer = array.buffer();
        resources.add(buffer, array.type().destruct_cb(), "Constant folded value");

        constant_fn = &resources.construct<MF_GenericConstantValue>(
            "Constant folded function", cpp_type, buffer);
        break;
      }
      case MFDataType::Vector: {
        GenericVectorArray &vector_array = params_builder.computed_vector_array(param_index);
        GenericArrayRef array = vector_array[0];
        constant_fn = &resources.construct<MF_GenericConstantVector>("Constant folded function",
                                                                     array);
        break;
      }
    }

    MFBuilderFunctionNode &folded_node = network_builder.add_function(*constant_fn);

    MFBuilderOutputSocket &original_socket =
        *dummy_nodes_to_compute[param_index]->input(0).origin();

    BLI::ScopedVector<MFBuilderInputSocket *> targets_copy = original_socket.targets();
    for (MFBuilderInputSocket *target : targets_copy) {
      network_builder.relink_origin(folded_node.output(0), *target);
    }
  }

  for (MFBuilderDummyNode *dummy_node : dummy_nodes_to_compute) {
    network_builder.remove_node(*dummy_node);
  }
}

}  // namespace FN
