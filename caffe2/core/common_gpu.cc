#include "caffe2/core/common_gpu.h"

#include <atomic>
#include <sstream>

#include "caffe2/core/init.h"

namespace caffe2 {

int NumCudaDevices() {
  static int count = -1;
  if (count < 0) {
    auto err = cudaGetDeviceCount(&count);
    if (err == cudaErrorNoDevice || err == cudaErrorInsufficientDriver) {
      count = 0;
    } else {
      // cudaGetDeviceCount() should only return the above two errors. If
      // there are other kinds of errors, maybe you have called some other
      // cuda functions before HasCudaGPU().
      CAFFE_CHECK(err == cudaSuccess)
          << "Unexpected error from cudaGetDeviceCount(). Did you run some "
             "cuda functions before calling NumCudaDevices() that might "
             "have already set an error?";
    }
  }
  return count;
}

namespace {
int gDefaultGPUID = 0;
}  // namespace

void SetDefaultGPUID(const int deviceid) {
  CAFFE_CHECK_LT(deviceid, NumCudaDevices())
      << "The default gpu id should be smaller than the number of gpus "
         "on this machine: " << deviceid << " vs " << NumCudaDevices();
  gDefaultGPUID = deviceid;
}
int GetDefaultGPUID() { return gDefaultGPUID; }

int GetCurrentGPUID() {
  int gpu_id = 0;
  CUDA_CHECK(cudaGetDevice(&gpu_id));
  return gpu_id;
}

int GetGPUIDForPointer(const void* ptr) {
  cudaPointerAttributes attr;
  CUDA_CHECK(cudaPointerGetAttributes(&attr, ptr));
  return attr.device;
}

const cudaDeviceProp& GetDeviceProperty(const int deviceid) {
  static vector<cudaDeviceProp> props;
  CAFFE_CHECK_LT(deviceid, NumCudaDevices())
      << "The gpu id should be smaller than the number of gpus "
         "on this machine: " << deviceid << " vs " << NumCudaDevices();
  if (props.size() == 0) {
    props.resize(NumCudaDevices());
    for (int i = 0; i < NumCudaDevices(); ++i) {
      CUDA_CHECK(cudaGetDeviceProperties(&props[i], i));
    }
  }
  return props[deviceid];
}

void DeviceQuery(const int device) {
  const cudaDeviceProp& prop = GetDeviceProperty(device);
  std::stringstream ss;
  ss << std::endl;
  ss << "Device id:                     " << device << std::endl;
  ss << "Major revision number:         " << prop.major << std::endl;
  ss << "Minor revision number:         " << prop.minor << std::endl;
  ss << "Name:                          " << prop.name << std::endl;
  ss << "Total global memory:           " << prop.totalGlobalMem << std::endl;
  ss << "Total shared memory per block: " << prop.sharedMemPerBlock
     << std::endl;
  ss << "Total registers per block:     " << prop.regsPerBlock << std::endl;
  ss << "Warp size:                     " << prop.warpSize << std::endl;
  ss << "Maximum memory pitch:          " << prop.memPitch << std::endl;
  ss << "Maximum threads per block:     " << prop.maxThreadsPerBlock
     << std::endl;
  ss << "Maximum dimension of block:    "
     << prop.maxThreadsDim[0] << ", " << prop.maxThreadsDim[1] << ", "
     << prop.maxThreadsDim[2] << std::endl;
  ss << "Maximum dimension of grid:     "
     << prop.maxGridSize[0] << ", " << prop.maxGridSize[1] << ", "
     << prop.maxGridSize[2] << std::endl;
  ss << "Clock rate:                    " << prop.clockRate << std::endl;
  ss << "Total constant memory:         " << prop.totalConstMem << std::endl;
  ss << "Texture alignment:             " << prop.textureAlignment << std::endl;
  ss << "Concurrent copy and execution: "
     << (prop.deviceOverlap ? "Yes" : "No") << std::endl;
  ss << "Number of multiprocessors:     " << prop.multiProcessorCount
     << std::endl;
  ss << "Kernel execution timeout:      "
     << (prop.kernelExecTimeoutEnabled ? "Yes" : "No") << std::endl;
  CAFFE_LOG_INFO << ss.str();
  return;
}

bool GetCudaPeerAccessPattern(vector<vector<bool> >* pattern) {
  int gpu_count;
  if (cudaGetDeviceCount(&gpu_count) != cudaSuccess) return false;
  pattern->clear();
  pattern->resize(gpu_count, vector<bool>(gpu_count, false));
  for (int i = 0; i < gpu_count; ++i) {
    for (int j = 0; j < gpu_count; ++j) {
      int can_access = true;
      if (i != j) {
        if (cudaDeviceCanAccessPeer(&can_access, i, j)
                 != cudaSuccess) {
          return false;
        }
      }
      (*pattern)[i][j] = static_cast<bool>(can_access);
    }
  }
  return true;
}

const char* cublasGetErrorString(cublasStatus_t error) {
  switch (error) {
  case CUBLAS_STATUS_SUCCESS:
    return "CUBLAS_STATUS_SUCCESS";
  case CUBLAS_STATUS_NOT_INITIALIZED:
    return "CUBLAS_STATUS_NOT_INITIALIZED";
  case CUBLAS_STATUS_ALLOC_FAILED:
    return "CUBLAS_STATUS_ALLOC_FAILED";
  case CUBLAS_STATUS_INVALID_VALUE:
    return "CUBLAS_STATUS_INVALID_VALUE";
  case CUBLAS_STATUS_ARCH_MISMATCH:
    return "CUBLAS_STATUS_ARCH_MISMATCH";
  case CUBLAS_STATUS_MAPPING_ERROR:
    return "CUBLAS_STATUS_MAPPING_ERROR";
  case CUBLAS_STATUS_EXECUTION_FAILED:
    return "CUBLAS_STATUS_EXECUTION_FAILED";
  case CUBLAS_STATUS_INTERNAL_ERROR:
    return "CUBLAS_STATUS_INTERNAL_ERROR";
#if CUDA_VERSION >= 6000
  case CUBLAS_STATUS_NOT_SUPPORTED:
    return "CUBLAS_STATUS_NOT_SUPPORTED";
#if CUDA_VERSION >= 6050
  case CUBLAS_STATUS_LICENSE_ERROR:
    return "CUBLAS_STATUS_LICENSE_ERROR";
#endif  // CUDA_VERSION >= 6050
#endif  // CUDA_VERSION >= 6000
  }
  // To suppress compiler warning.
  return "Unrecognized cublas error string";
}

const char* curandGetErrorString(curandStatus_t error) {
  switch (error) {
  case CURAND_STATUS_SUCCESS:
    return "CURAND_STATUS_SUCCESS";
  case CURAND_STATUS_VERSION_MISMATCH:
    return "CURAND_STATUS_VERSION_MISMATCH";
  case CURAND_STATUS_NOT_INITIALIZED:
    return "CURAND_STATUS_NOT_INITIALIZED";
  case CURAND_STATUS_ALLOCATION_FAILED:
    return "CURAND_STATUS_ALLOCATION_FAILED";
  case CURAND_STATUS_TYPE_ERROR:
    return "CURAND_STATUS_TYPE_ERROR";
  case CURAND_STATUS_OUT_OF_RANGE:
    return "CURAND_STATUS_OUT_OF_RANGE";
  case CURAND_STATUS_LENGTH_NOT_MULTIPLE:
    return "CURAND_STATUS_LENGTH_NOT_MULTIPLE";
  case CURAND_STATUS_DOUBLE_PRECISION_REQUIRED:
    return "CURAND_STATUS_DOUBLE_PRECISION_REQUIRED";
  case CURAND_STATUS_LAUNCH_FAILURE:
    return "CURAND_STATUS_LAUNCH_FAILURE";
  case CURAND_STATUS_PREEXISTING_FAILURE:
    return "CURAND_STATUS_PREEXISTING_FAILURE";
  case CURAND_STATUS_INITIALIZATION_FAILED:
    return "CURAND_STATUS_INITIALIZATION_FAILED";
  case CURAND_STATUS_ARCH_MISMATCH:
    return "CURAND_STATUS_ARCH_MISMATCH";
  case CURAND_STATUS_INTERNAL_ERROR:
    return "CURAND_STATUS_INTERNAL_ERROR";
  }
  // To suppress compiler warning.
  return "Unrecognized curand error string";
}

bool Caffe2InitializeCuda(int*, char***) {
  static bool g_initialization_function_called = false;
  if (g_initialization_function_called == true) {
    CAFFE_VLOG(1) << "Initialization already called. Ignoring duplicated calls.";
    return true;
  }
  g_initialization_function_called = true;
  // If the current run does not have any cuda devices, do nothing.
  if (!HasCudaGPU()) {
    CAFFE_VLOG(1) << "No cuda gpu present. Skipping.";
    return true;
  }
  // Check if the number of GPUs matches the expected compile-time max number
  // of GPUs.
  CAFFE_CHECK_LE(NumCudaDevices(), CAFFE2_COMPILE_TIME_MAX_GPUS)
      << "Number of CUDA devices on the machine is larger than the compiled "
         "max number of gpus expected ("
      << CAFFE2_COMPILE_TIME_MAX_GPUS
      << "). Increase that and recompile the caffe binary.";
  // Save the current device so we can restore it after moving across
  // different devices.
  int init_device;
  CUDA_CHECK(cudaGetDevice(&init_device));

  for (int i = 0; i < NumCudaDevices(); ++i) {
    auto err = cudaSetDevice(i);
    if (err != cudaSuccess) {
      CAFFE_LOG_WARNING
          << "Cannot use device " << i
          << "due to the following error: " << cudaGetErrorString(err);
      continue;
    }
    // Enable peer access.
    for (int j = 0; j < NumCudaDevices(); ++j) {
      if (i == j) continue;
      int can_access;
      CUDA_CHECK(cudaDeviceCanAccessPeer(&can_access, i, j));
      if (can_access) {
        CAFFE_VLOG(1) << "Enabling peer access from " << i << " to " << j;
        // Note: just for future reference, the 0 here is not a gpu id, it is
        // a reserved flag for cudaDeviceEnablePeerAccess that should always be
        // zero currently.
        CUDA_CHECK(cudaDeviceEnablePeerAccess(j, 0));
      }
    }
  }
  // Restore the current device.
  CUDA_CHECK(cudaSetDevice(init_device));
  return true;
}

REGISTER_CAFFE2_INIT_FUNCTION(Caffe2InitializeCuda,
                              &Caffe2InitializeCuda,
                              "Enable cuda for caffe2.");

}  // namespace caffe2
