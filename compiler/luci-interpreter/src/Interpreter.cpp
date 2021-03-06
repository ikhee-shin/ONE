/*
 * Copyright (c) 2020 Samsung Electronics Co., Ltd. All Rights Reserved
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "luci_interpreter/Interpreter.h"

#include "KernelBuilder.h"
#include "KernelMap.h"
#include "TensorMap.h"

#include <loco/IR/Algorithm.h>

#include <algorithm>
#include <stdexcept>

namespace luci_interpreter
{

template <typename NodeT> static Shape getNodeShape(const NodeT *node)
{
  Shape shape(node->rank());
  for (uint32_t i = 0; i < node->rank(); ++i)
  {
    shape.dim(i) = node->dim(i).value();
  }
  return shape;
}

template <DataType DT>
static const void *getNodeDataImpl(const luci::CircleConst *node, size_t *data_size)
{
  const size_t element_size = getDataTypeSize(DT);
  const int32_t num_elements = node->size<DT>();

  *data_size = num_elements * element_size;
  // FIXME There is no good way to get the pointer to the data currently.
  return &node->at<DT>(0);
}

static const void *getNodeData(const luci::CircleConst *node, size_t *data_size)
{
  switch (node->dtype())
  {
    case DataType::U8:
      return getNodeDataImpl<DataType::U8>(node, data_size);
    case DataType::FLOAT32:
      return getNodeDataImpl<DataType::FLOAT32>(node, data_size);
    case DataType::S32:
      return getNodeDataImpl<DataType::S32>(node, data_size);
    default:
      throw std::runtime_error("Unsupported type.");
  }
}

static bool isExecutableNode(const luci::CircleNode *node)
{
  switch (node->opcode())
  {
    // These nodes denote inputs / outputs of a graph.
    case luci::CircleOpcode::CONST:
    case luci::CircleOpcode::CIRCLEINPUT:
    case luci::CircleOpcode::CIRCLEOUTPUT:
    // The following nodes denote outputs of multiple-output nodes.
    case luci::CircleOpcode::CIRCLESPLITOUT:
    case luci::CircleOpcode::CIRCLEUNPACKOUT:
      return false;
    default:
      return true;
  }
}

static bool isTensorProducingNode(const luci::CircleNode *node)
{
  switch (node->opcode())
  {
    // Output nodes do not produce tensors.
    case luci::CircleOpcode::CIRCLEOUTPUT:
    // The following nodes are multiple-output nodes. They do not produce tensors, the tensors
    // are produced by the corresponding *Out nodes instead.
    case luci::CircleOpcode::SPLIT:
    case luci::CircleOpcode::UNPACK:
      return false;
    default:
      return true;
  }
}

void Interpreter::createTensors(const loco::Graph *graph)
{
  for (uint32_t i = 0; i < graph->nodes()->size(); ++i)
  {
    const auto *node = loco::must_cast<const luci::CircleNode *>(graph->nodes()->at(i));

    if (!isTensorProducingNode(node))
      continue;

    // Only Input and Const nodes have shapes. Shapes of intermediate tensors will be inferred.
    Shape shape{};
    if (const auto *input_node = dynamic_cast<const luci::CircleInput *>(node))
    {
      shape = getNodeShape(input_node);
    }
    else if (const auto *const_node = dynamic_cast<const luci::CircleConst *>(node))
    {
      shape = getNodeShape(const_node);
    }

    AffineQuantization quantization;
    if (node->quantparam() != nullptr)
    {
      const luci::CircleQuantParam *params = node->quantparam();
      quantization.scale.assign(params->scale.cbegin(), params->scale.cend());
      quantization.zero_point.assign(params->zerop.cbegin(), params->zerop.cend());
    }

    auto tensor = std::make_unique<Tensor>(node->dtype(), std::move(shape), std::move(quantization),
                                           node->name());

    if (const auto *const_node = dynamic_cast<const luci::CircleConst *>(node))
    {
      size_t data_size{};
      const void *const_data = getNodeData(const_node, &data_size);
      tensor->writeData(const_data, data_size);
    }

    _tensor_map->setTensor(node, std::move(tensor));
  }
}

void Interpreter::createKernels(const loco::Graph *graph)
{
  KernelBuilder kernel_builder(*_tensor_map);

  for (uint32_t i = 0; i < graph->nodes()->size(); ++i)
  {
    const auto *node = loco::must_cast<const luci::CircleNode *>(graph->nodes()->at(i));

    if (isExecutableNode(node))
      _kernel_map->setKernel(node, node->accept(&kernel_builder));
  }
}

Interpreter::Interpreter(const luci::Module *module)
{
  if (module->size() > 1)
  {
    throw std::runtime_error("Models with multiple subgraphs are not yet supported.");
  }

  _main_graph = module->graph();

  _tensor_map = std::make_unique<TensorMap>();
  _kernel_map = std::make_unique<KernelMap>();

  createTensors(_main_graph);
  createKernels(_main_graph);

  // Configure the kernels, e.g. resize the tensors that they produce and do other kernel dependent
  // initialization. This has to be done in execution order, because configuration of a kernel may
  // (and in most cases does) depend on configurations of its predecessors.
  // TODO Some kernels (ex. Reshape, Pad) need some of their input tensors (ex 'shape', 'paddings')
  //  to be known in order to configure properly. This means that 'configure' and 'execute' steps
  //  should be interleaved. For now such 'dynamic' tensors are not supported.
  for (const loco::Node *loco_node :
       loco::postorder_traversal(loco::output_nodes(const_cast<loco::Graph *>(_main_graph))))
  {
    const auto *node = loco::must_cast<const luci::CircleNode *>(loco_node);

    if (isExecutableNode(node))
    {
      Kernel *kernel = _kernel_map->getKernel(node);
      kernel->configure();
    }
  }
}

Interpreter::~Interpreter() = default;

void Interpreter::writeInputTensor(const luci::CircleInput *input_node, const void *data,
                                   size_t data_size)
{
  Tensor *tensor = _tensor_map->getTensor(input_node);
  if (tensor == nullptr)
  {
    const std::string &name = input_node->name();
    throw std::runtime_error("Cannot find tensor for input node named \"" + name + "\".");
  }
  tensor->writeData(data, data_size);
}

void Interpreter::readOutputTensor(const luci::CircleOutput *output_node, void *data,
                                   size_t data_size)
{
  Tensor *tensor = _tensor_map->getTensor(output_node->from());
  if (tensor == nullptr)
  {
    const std::string &name = output_node->name();
    throw std::runtime_error("Cannot find tensor for output node named \"" + name + "\".");
  }
  tensor->readData(data, data_size);
}

void Interpreter::interpret()
{
  for (const loco::Node *loco_node :
       loco::postorder_traversal(loco::output_nodes(const_cast<loco::Graph *>(_main_graph))))
  {
    const auto *node = loco::must_cast<const luci::CircleNode *>(loco_node);

    if (isExecutableNode(node))
    {
      Kernel *kernel = _kernel_map->getKernel(node);
      kernel->execute();
    }

    // Notify the observers that the node's output tensor has changed.
    if (isTensorProducingNode(node))
    {
      for (ExecutionObserver *observer : _observers)
      {
        observer->postTensorWrite(node, _tensor_map->getTensor(node));
      }
    }
  }
}

void Interpreter::attachObserver(ExecutionObserver *observer)
{
  if (std::find(_observers.cbegin(), _observers.cend(), observer) != _observers.cend())
    throw std::runtime_error("Observer is already attached.");
  _observers.push_back(observer);
}

ExecutionObserver::~ExecutionObserver() = default;

void ExecutionObserver::postTensorWrite(const luci::CircleNode *, const Tensor *) {}

} // namespace luci_interpreter
