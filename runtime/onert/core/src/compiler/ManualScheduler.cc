/*
 * Copyright (c) 2019 Samsung Electronics Co., Ltd. All Rights Reserved
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ManualScheduler.h"
#include "ir/OpCode.h"
#include "ir/Operations.Include.h"
#include "backend/Backend.h"
#include "backend/IConfig.h"
#include "compiler/BackendManager.h"
#include "util/ConfigSource.h"
#include "util/logging.h"
#include "misc/string_helpers.h"

namespace onert
{
namespace compiler
{

ManualScheduler::ManualScheduler(const backend::BackendContexts &backend_contexts,
                                 const compiler::CompilerOptions &options)
    : _backend_contexts{backend_contexts}, _options{options}
{
}

std::unique_ptr<BackendResolver> ManualScheduler::schedule(const ir::Graph &graph)
{
  const auto &manual_options = _options.manual_scheduler_options;
  auto backend_resolver = std::make_unique<compiler::BackendResolver>();

  // This fallback will be used for unavailable backends
  auto fallback = [&]() -> const backend::Backend * {
    for (auto backend_id : _options.backend_list)
    {
      auto backend = resolveBackend(backend_id);
      if (backend)
        return backend;
    }
    return nullptr;
  }();
  assert(fallback != nullptr); // There must be at least one fallback

  // 1. Backend for All operations
  const backend::Backend *backend_all = resolveBackend(manual_options.backend_for_all, fallback);
  VERBOSE(ManualScheduler) << "Default backend for all ops: " << backend_all->config()->id()
                           << std::endl;

  graph.operations().iterate([&](const ir::OperationIndex &index, const ir::Operation &) {
    backend_resolver->setBackend(index, backend_all);
  });

  // 2. Backend per operation type
  std::unordered_map<ir::OpCode, backend::Backend *> op_type_map;
  for (auto &pair : manual_options.opcode_to_backend)
  {
    op_type_map.emplace(
        pair.first, BackendManager::get().get(
                        pair.second)); // TODO Ensure this backend is available in backend contexts
  }
  // By default, Custom uses cpu backend
  op_type_map[ir::OpCode::Custom] = BackendManager::get().get("cpu");

  graph.operations().iterate([&](const ir::OperationIndex &index, const ir::Operation &operation) {
    auto itr = op_type_map.find(operation.opcode());
    if (itr != op_type_map.end())
    {
      backend_resolver->setBackend(index, itr->second);
    }
  });

  // 3. Backend per operation
  for (auto &pair : manual_options.index_to_backend)
  {
    const auto &key = pair.first;
    const auto &val = pair.second;

    try
    {
      graph.operations().at(key); // Check if exist, or this will throw
      backend_resolver->setBackend(
          key, BackendManager::get().get(
                   val)); // TODO Ensure this backend is available in backend contexts
    }
    catch (...)
    {
      VERBOSE(ManualScheduler) << "Invalid value while OperationIndex to Backend mapping : @"
                               << key.value() << " -> \"" << val << "\"" << std::endl;
    }
  }

  // 4. Operations that are specially handled
  //    All configuration above will be ignored(overwritten)
  op_type_map[ir::OpCode::Permute] = BackendManager::get().get("cpu");

  // Dump final assignment
  backend_resolver->iterate([&](const ir::OperationIndex &index, const backend::Backend &backend) {
    VERBOSE(ManualScheduler) << "backend for operation #" << index.value() << ": "
                             << backend.config()->id() << std::endl;
  });

  return backend_resolver;
}

const backend::Backend *ManualScheduler::resolveBackend(const std::string &id,
                                                        const backend::Backend *fallback)
{
  // Ensure if the backend is available in the backend
  const backend::Backend *backend = BackendManager::get().get(id);
  if (!backend || _backend_contexts.find(backend) == _backend_contexts.end())
  {
    backend = fallback;
  }
  return backend;
}

} // namespace compiler
} // namespace onert
