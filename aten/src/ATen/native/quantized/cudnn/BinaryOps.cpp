#ifdef USE_CUDA
#include <ATen/cuda/CUDAConfig.h>  // for the definition of AT_CUDNN_ENABLED

#if AT_CUDNN_ENABLED()
#include <ATen/native/cudnn/Macros.h>
#if HAS_CUDNN_V8()

#include <ATen/core/TensorBase.h>
#include <ATen/core/TensorBody.h>
#include <ATen/cuda/Exceptions.h>
#include <ATen/cudnn/Handle.h>
#include <ATen/native/quantized/cudnn/utils.h>
#include <ATen/native/utils/ParamsHash.h>
#include <ATen/TensorUtils.h>
#include <c10/core/QScheme.h>
#include <torch/library.h>

#include <unordered_map>

// FIXME: make this thread-safe by reusing the benchmark cache in Conv_v7.cpp
namespace {
struct CacheKey {
  uint8_t input_a_alignment;
  uint8_t input_b_alignment;
  uint8_t output_alignment;
};
std::unordered_map<CacheKey, cudnn_frontend::ManagedOpaqueDescriptor, at::native::ParamsHash<CacheKey>, at::native::ParamsEqual<CacheKey>> execution_plan_cache;
}

namespace at {
namespace native {
namespace {
// TODO: this is also in qadd.cpp and some other cpp files in quantized/cpu/. I think we should
// move everything into a utilities file in quantized/ directory later.
inline void check_inputs(const Tensor& qa, const Tensor& qb) {
  TORCH_CHECK(
      qa.qscheme() == kPerTensorAffine,
      "Only per tensor quantization is suported in Add.");
  TORCH_CHECK(
      qa.qscheme() == qb.qscheme(),
      "Both inputs to Add must have the same quantization shceme.");
  TORCH_CHECK(
      qa.scalar_type() == qb.scalar_type(),
      "Add operands should have same data type.");
}

// currently we only support int8 symmetric (zero_point = 0 for inputs and output) quantized add
template <bool kReluFused = false>
Tensor add(Tensor qa, Tensor qb, double output_scale, int64_t output_zero_point) {
  std::cout << "cuda add" << std::endl;
  if (qa.numel() == 0) {
    return Tensor{};
  }
  // TODO: add shape checking? I think this is contingent on whether we support broadcasting in qadd, so maybe leave this out for now?
  check_inputs(qa, qb);

  // cudnn expects tensors to be at least 3D. So we will append dummy dimensions if the input tensors are not at least 3D
  if (qa.dim() < 3) {
    std::vector<int64_t> new_sizes{qa.sizes().vec()};
    while (new_sizes.size() < 3) {
      new_sizes.emplace_back(1);
    }
    qa = at::native::view(qa, new_sizes);
    qb = at::native::view(qb, new_sizes);
  }


  at::Tensor add_output = at::empty(qa.sizes(), at::device(at::kCUDA).dtype(at::kFloat));
  auto requantize_multiplier = qa.q_scale() * qb.q_scale() / output_scale;
  at::Tensor quantized_output = at::_empty_affine_quantized(
      qa.sizes(),
      at::device(at::kCUDA).dtype(at::ScalarType::QInt8),
      output_scale,
      output_zero_point);
  // TODO: When cudnn enables support for broadcasting, we can remove this tensor
  at::Tensor requantize_multiplier_tensor = at::empty(quantized_output.sizes(), at::device(at::kCUDA).dtype(at::kFloat));
  requantize_multiplier_tensor.fill_(requantize_multiplier);

  cudnnHandle_t handle = at::native::getCudnnHandle();
  CacheKey key;
  bool deterministic{true};
  bool allow_tf32{false};

  key.input_a_alignment = cudnn_utils::getAlignment(qa);
  key.input_b_alignment = cudnn_utils::getAlignment(qb);
  key.output_alignment = cudnn_utils::getAlignment(add_output);

  auto run = [&](cudnn_frontend::ManagedOpaqueDescriptor plan_desc) {
    auto workspace_size = 0;
    auto workspace = at::empty({workspace_size}, qa.options().dtype(at::kByte));
    std::vector<void *> data_ptrs;
    std::vector<int64_t> uids;
    data_ptrs.reserve(10);
    uids.reserve(10);
    data_ptrs = {reinterpret_cast<int8_t*>(qa.data_ptr()), reinterpret_cast<int8_t*>(qb.data_ptr()),
                                           add_output.data_ptr(), requantize_multiplier_tensor.data_ptr(),
                                           reinterpret_cast<int8_t*>(quantized_output.data_ptr())};
    uids = {'a', 'b', 'c', 'r', 'q'};
    if (kReluFused) {
        data_ptrs.emplace_back(add_output.data_ptr()),
        uids.emplace_back('f');
    }

    auto variantPack = cudnn_frontend::VariantPackBuilder()
      .setWorkspacePointer(workspace.data_ptr())
      .setDataPointers(uids.size(), data_ptrs.data())
      .setUids(uids.size(), uids.data())
      .build();
    auto variant_pack_desc = variantPack.get_raw_desc();
    AT_CUDNN_CHECK(cudnnBackendExecute(handle, plan_desc->get_backend_descriptor(), variant_pack_desc));
  };

  auto search = execution_plan_cache.find(key);
  if (search != execution_plan_cache.end()) {
    cudnn_frontend::ManagedOpaqueDescriptor plan_desc = search->second;
    run(plan_desc);
    return quantized_output;
  }

  // add_op computes qa + qb  (both inputs are int8)
  // add_output is a fp32 tensor for accumulation purposes
  auto add_op = cudnn_frontend::OperationBuilder(CUDNN_BACKEND_OPERATION_POINTWISE_DESCRIPTOR)
      .setxDesc(cudnn_utils::getTensorDescriptor(qa.sizes(), qa.strides(), CUDNN_DATA_INT8, 'a', key.input_a_alignment))
      .setbDesc(cudnn_utils::getTensorDescriptor(qb.sizes(), qb.strides(), CUDNN_DATA_INT8, 'b', key.input_b_alignment))
      .setyDesc(cudnn_utils::getTensorDescriptor(add_output, 'c', key.output_alignment))
      .setpwDesc(cudnn_utils::getPointWiseAddDescriptor(at::native::getCudnnDataType(add_output)))
      .build();

  // relu_op computes
  // relu( (qa + qb) * (qa_scale * qb_scale) / out_scale )
  // output is a fp32 tensor
  c10::optional<cudnn_frontend::Operation> relu_op;
  if (kReluFused) {
    // we use inplace operation here where the output is assigned to the input
    relu_op.emplace(cudnn_frontend::OperationBuilder(CUDNN_BACKEND_OPERATION_POINTWISE_DESCRIPTOR)
      .setxDesc(add_op.getOutputTensor())
      .setyDesc(cudnn_utils::getTensorDescriptor(add_output, 'f', key.output_alignment))
      .setpwDesc(cudnn_utils::getPointWiseReluDescriptor(at::native::getCudnnDataType(add_output)))
      .build());
  }

  // requant_op computes
  // (qa_int8 + qb_int8) * (qa_scale * qb_scale) / out_scale
  // where requantize_multiplier = (qa_scale * qb_scale) / out_scale
  auto requant_op = cudnn_frontend::OperationBuilder(CUDNN_BACKEND_OPERATION_POINTWISE_DESCRIPTOR)
    .setxDesc(kReluFused ? relu_op.value().getOutputTensor() : add_op.getOutputTensor())
    .setbDesc(cudnn_utils::getTensorDescriptor(requantize_multiplier_tensor, 'r', cudnn_utils::getAlignment(requantize_multiplier_tensor)))
    .setyDesc(cudnn_utils::getTensorDescriptor(quantized_output.sizes(), quantized_output.strides(), CUDNN_DATA_INT8, 'q', cudnn_utils::getAlignment(quantized_output)))
    .setpwDesc(cudnn_utils::getPointWiseMulDescriptor(at::native::getCudnnDataType(requantize_multiplier_tensor)))
    .build();

  std::vector<cudnn_frontend::Operation const *> ops{&add_op};
  if (kReluFused) {
    ops.emplace_back(&(relu_op.value()));
  }
  ops.emplace_back(&requant_op);

  auto opGraph = cudnn_frontend::OperationGraphBuilder()
      .setHandle(handle)
      .setOperationGraph(ops.size(), ops.data())
      .build();
  // std::cout << "opGraph: " << opGraph.describe() << std::endl;

  auto heuristics = cudnn_frontend::EngineHeuristicsBuilder()
      .setOperationGraph(opGraph)
      .setHeurMode(CUDNN_HEUR_MODE_INSTANT)
      .build();
  auto fallback = cudnn_frontend::EngineFallbackListBuilder()
                    .setOperationGraph(opGraph)
                    .setOperation(CUDNN_BACKEND_OPERATION_CONVOLUTION_FORWARD_DESCRIPTOR)
                    .build();

  auto& engine_configs = heuristics.getEngineConfig(heuristics.getEngineConfigCount());
  auto& fallback_list = fallback.getFallbackList();

  cudnn_frontend::EngineConfigList filtered_configs;
  cudnn_utils::filterEngineConfigs(engine_configs, filtered_configs, deterministic, allow_tf32, at::kChar);
  cudnn_utils::filterEngineConfigs(fallback_list, filtered_configs, deterministic, allow_tf32, at::kChar);
  for (auto &cfg : engine_configs) {
    std::cout << "cfg" << std::endl;
    try {
      auto plan = cudnn_frontend::ExecutionPlanBuilder()
        .setHandle(handle)
        .setEngineConfig(cfg)
        .build();
      auto plan_desc = plan.get_desc();
      run(plan_desc);
      execution_plan_cache[key] = plan_desc;
      std::cout << "here" << std::endl;
      return quantized_output;
    } catch (cudnn_frontend::cudnnException &e) {std::cout << "cudnn error:" << e.what() << std::endl;} catch(c10::CuDNNError &e) { std::cout << "other error" << e.what() << std::endl;}
  }

  TORCH_CHECK(false, "Unable to find an engine to execute this computation");
}

TORCH_LIBRARY_IMPL(quantized, QuantizedCUDA, m) {
  m.impl(TORCH_SELECTIVE_NAME("quantized::add"), TORCH_FN(add</*ReLUFused=*/false>));
  m.impl(TORCH_SELECTIVE_NAME("quantized::add_relu"), TORCH_FN(add</*ReLUFused=*/true>));
}

} // namespace
} // namespace native
} // namespace at

#endif  // HAS_CUDNN_V8
#endif  // AT_CUDNN_ENABLED
#endif  // USE_CUDA
