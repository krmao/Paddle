/* Copyright (c) 2016 PaddlePaddle Authors. All Rights Reserve.
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
    http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "paddle/fluid/platform/device_context.h"
#include "paddle/fluid/memory/memory.h"

namespace paddle {
namespace platform {

DeviceContextPool* DeviceContextPool::pool = nullptr;

const platform::DeviceContext* DeviceContextPool::Get(
    const platform::Place& place) {
  auto it = device_contexts_.find(place);
  if (it == device_contexts_.end()) {
    PADDLE_THROW(
        "'Place' is not supported, Please re-compile with WITH_GPU "
        "option");
  }
  return it->second;
}

DeviceContextPool::DeviceContextPool(
    const std::vector<platform::Place>& places) {
  PADDLE_ENFORCE_GT(places.size(), 0);
  for (size_t i = 0; i < places.size(); i++) {
    if (platform::is_cpu_place(places[i])) {
      device_contexts_.emplace(places[i],
                               new platform::CPUDeviceContext(
                                   boost::get<platform::CPUPlace>(places[i])));
    } else if (platform::is_gpu_place(places[i])) {
#ifdef PADDLE_WITH_CUDA
      device_contexts_.emplace(places[i],
                               new platform::CUDADeviceContext(
                                   boost::get<platform::CUDAPlace>(places[i])));
#else
      PADDLE_THROW(
          "'CUDAPlace' is not supported, Please re-compile with WITH_GPU "
          "option");
#endif
    }
  }
}

CPUDeviceContext::CPUDeviceContext() {
  eigen_device_.reset(new Eigen::DefaultDevice());
}

CPUDeviceContext::CPUDeviceContext(CPUPlace place) : place_(place) {
  eigen_device_.reset(new Eigen::DefaultDevice());
}

Eigen::DefaultDevice* CPUDeviceContext::eigen_device() const {
  return eigen_device_.get();
}

Place CPUDeviceContext::GetPlace() const { return place_; }

#ifdef PADDLE_WITH_CUDA

class EigenCudaStreamDevice : public Eigen::StreamInterface {
 public:
  EigenCudaStreamDevice() : scratch_(nullptr), semaphore_(nullptr) {
    Eigen::initializeDeviceProp();
  }
  ~EigenCudaStreamDevice() override {}

  void Reinitialize(const cudaStream_t* cuda_stream, CUDAPlace place) {
    stream_ = cuda_stream;
    place_ = place;
    device_prop_ = &Eigen::m_deviceProperties[place.device];
  }

  const cudaStream_t& stream() const override { return *stream_; }

  const cudaDeviceProp& deviceProperties() const override {
    return *device_prop_;
  }

  void* allocate(size_t num_bytes) const override {
    return paddle::memory::Alloc(place_, num_bytes);
  }

  void deallocate(void* buffer) const override {
    paddle::memory::Free(place_, buffer);
  }

  void* scratchpad() const override {
    if (scratch_ == NULL) {
      scratch_ = allocate(Eigen::kCudaScratchSize + sizeof(unsigned int));
    }
    return scratch_;
  }

  unsigned int* semaphore() const override {
    if (semaphore_ == NULL) {
      char* scratch =
          static_cast<char*>(scratchpad()) + Eigen::kCudaScratchSize;
      semaphore_ = reinterpret_cast<unsigned int*>(scratch);
      PADDLE_ENFORCE(
          cudaMemsetAsync(semaphore_, 0, sizeof(unsigned int), *stream_));
    }
    return semaphore_;
  }

 private:
  CUDAPlace place_;
  const cudaStream_t* stream_;         // not owned;
  const cudaDeviceProp* device_prop_;  // not owned;
  mutable void* scratch_;
  mutable unsigned int* semaphore_;
};

CUDADeviceContext::CUDADeviceContext(CUDAPlace place) : place_(place) {
  SetDeviceId(place_.device);
  PADDLE_ENFORCE(cudaStreamCreate(&stream_));
  eigen_stream_.reset(new EigenCudaStreamDevice());
  eigen_stream_->Reinitialize(&stream_, place);
  eigen_device_.reset(new Eigen::GpuDevice(eigen_stream_.get()));
  PADDLE_ENFORCE(dynload::cublasCreate(&cublas_handle_));
  PADDLE_ENFORCE(dynload::cublasSetStream(cublas_handle_, stream_));
  if (dynload::HasCUDNN()) {
    PADDLE_ENFORCE(dynload::cudnnCreate(&cudnn_handle_));
    PADDLE_ENFORCE(dynload::cudnnSetStream(cudnn_handle_, stream_));
  } else {
    cudnn_handle_ = nullptr;
  }
}

CUDADeviceContext::~CUDADeviceContext() {
  SetDeviceId(place_.device);
  Wait();
  PADDLE_ENFORCE(dynload::cublasDestroy(cublas_handle_));
  if (cudnn_handle_ != nullptr) {
    PADDLE_ENFORCE(dynload::cudnnDestroy(cudnn_handle_));
  }
  eigen_stream_.reset();
  eigen_device_.reset();
  PADDLE_ENFORCE(cudaStreamDestroy(stream_));
}

Place CUDADeviceContext::GetPlace() const { return place_; }

void CUDADeviceContext::Wait() const {
  PADDLE_ENFORCE(cudaStreamSynchronize(stream_));
  PADDLE_ENFORCE(cudaGetLastError());
}

Eigen::GpuDevice* CUDADeviceContext::eigen_device() const {
  return eigen_device_.get();
}

cublasHandle_t CUDADeviceContext::cublas_handle() const {
  return cublas_handle_;
}

cudnnHandle_t CUDADeviceContext::cudnn_handle() const { return cudnn_handle_; }

cudaStream_t CUDADeviceContext::stream() const { return stream_; }

#endif

#ifdef PADDLE_WITH_MKLDNN
MKLDNNDeviceContext::MKLDNNDeviceContext(CPUPlace place)
    : CPUDeviceContext(place), ready_(false) {
  stream_.reset(new mkldnn::stream(mkldnn::stream::kind::eager));
  engine_.reset(new mkldnn::engine(mkldnn::engine::cpu, 0));
}

template <typename T>
void MKLDNNDeviceContext::AddElement(const std::string& op_key,
                                     const T& value) {
  if (GetElement<T>(op_key)) {
    return;
  }
  GetElementPool<T>().emplace(op_key, std::move(value));
}

template <typename T>
const T& MKLDNNDeviceContext::GetElement(const std::string& op_key) const {
  auto it = GetElementPool<T>().find(op_key);
  return it == GetElementPool<T>().end() ? nullptr : it->second;
}

template <>
const std::unordered_map<const std::string, const MKLDNNMemoryPtr,
                         std::hash<std::string>>&
MKLDNNDeviceContext::GetElementPool<MKLDNNMemoryPtr>() const {
  return memory_pool_;
}

template <>
const std::unordered_map<const std::string, const MKLDNNPrimitivePtr,
                         std::hash<std::string>>&
MKLDNNDeviceContext::GetElementPool<MKLDNNPrimitivePtr>() const {
  return primitive_pool_;
}

template <>
const std::unordered_map<const std::string, const MKLDNNPrimitiveDescPtr,
                         std::hash<std::string>>&
MKLDNNDeviceContext::GetElementPool<MKLDNNPrimitiveDescPtr>() const {
  return primitive_desc_pool_;
}

void MKLDNNDeviceContext::Execute(bool block) {
  if (pipeline_.empty()) {
    return;
  }
  ResetStream();
  stream_->submit(pipeline_).wait(block);
  ready_ = false;
  pipeline_.clear();
}

void MKLDNNDeviceContext::ResetStream() {
  if (ready_) {
    return;
  }
  // TODO(TJ): change me when mkldnn have specific method to reset this state
  stream_.reset(new mkldnn::stream(mkldnn::stream::kind::eager));
  ready_ = true;
}

#endif

}  // namespace platform
}  // namespace paddle
