#ifndef CAFFE2_UTILS_MKL_MKL_MEMORY_HPP_
#define CAFFE2_UTILS_MKL_MKL_MEMORY_HPP_

#include <string>
#include <vector>

#include "caffe2/utils/mkl/mkl_dnn_cppwrapper.h"

namespace caffe2 {
namespace mkl {

template <typename T>
class PrimitiveWrapper {
 public:
  PrimitiveWrapper() {}
  // Creates a primitive wrapper from an existing primitive. The wrapper
  // takes over ownership.
  explicit PrimitiveWrapper(dnnPrimitive_t primitive) : primitive_(primitive) {}

  template <typename Creator, typename FirstArg, typename... Args>
  PrimitiveWrapper(Creator creator, FirstArg&& arg, Args&&... args) {
    creator(&primitive_, arg, args...);
  }

  ~PrimitiveWrapper() {
    if (primitive_) {
      MKLDNN_CHECK(dnnDelete<T>(primitive_));
    }
  }

  template <typename Creator, typename... Args>
  void Reset(Creator creator, Args&&... args) {
    if (primitive_) {
      MKLDNN_SAFE_CALL(dnnDelete<T>(primitive_));
    }
    creator(&primitive_, args...);
  }

  operator dnnPrimitive_t() const {
    return primitive_;
  }

 private:
  dnnPrimitive_t primitive_ = 0;
  DISABLE_COPY_AND_ASSIGN(PrimitiveWrapper);
};

template <typename T>
class LayoutWrapper {
 public:
  LayoutWrapper() {}
  // Create a user layout from a TensorCPU with the given shapes.
  explicit LayoutWrapper(const TensorCPU& tensor) {
    Reset(tensor);
  }

  // Create an internal layout from the primitive and type.
  LayoutWrapper(const dnnPrimitive_t primitive, const dnnResourceType_t type) {
    Reset(primitive, type);
  }

  // Create a user layout from the given dimension, size and strides.
  LayoutWrapper(
      const size_t dimension,
      const size_t size[],
      const size_t strides[]) {
    Reset(dimension, size, strides);
  }

  // Destructs the layout wrapper.
  ~LayoutWrapper() {
    if (layout_)
      MKLDNN_CHECK(dnnLayoutDelete<T>(layout_));
  }

  // Create a user layout from a TensorCPU with the given shapes.
  void Reset(const TensorCPU& tensor) {
    if (layout_)
      MKLDNN_CHECK(dnnLayoutDelete<T>(layout_));
    size_t dimension = tensor.ndim();
    size_t size[dimension];
    size_t strides[dimension];
    for (int i = 0; i < dimension; ++i) {
      size[i] = tensor.dim(dimension - i - 1);
      strides[i] = (i == 0) ? 1 : strides[i - 1] * size[i - 1];
    }
    MKLDNN_SAFE_CALL(dnnLayoutCreate<T>(&layout_, dimension, size, strides));
  }

  // Create an internal layout from the primitive and type.
  void Reset(const dnnPrimitive_t primitive, const dnnResourceType_t type) {
    if (layout_)
      MKLDNN_CHECK(dnnLayoutDelete<T>(layout_));
    MKLDNN_SAFE_CALL(
        dnnLayoutCreateFromPrimitive<T>(&layout_, primitive, type));
  }

  // Create a user layout from the given dimension, size and strides.
  void
  Reset(const size_t dimension, const size_t size[], const size_t strides[]) {
    if (layout_)
      MKLDNN_CHECK(dnnLayoutDelete<T>(layout_));
    MKLDNN_SAFE_CALL(dnnLayoutCreate<T>(&layout_, dimension, size, strides));
  }

  operator dnnLayout_t() const {
    return layout_;
  }

 private:
  dnnLayout_t layout_ = 0;
  DISABLE_COPY_AND_ASSIGN(LayoutWrapper);
};

/**
 * @brief A wrapper around an opaque MKL internal resource that has certain
 * layouts and convertion primitives set up.
 */
template <typename T>
class MKLMemory {
 public:
  // Initializes an empty MKLMemory.
  MKLMemory() {}
  // Initialize an MKLMemory with the given size, strides, dnn
  // primitive and type.
  MKLMemory(
      const size_t dimension,
      const size_t size[],
      const size_t strides[],
      const dnnPrimitive_t primitive,
      const dnnResourceType_t type,
      bool share_mem_if_possible = false) {
    dims_.resize(dimension);
    for (int i = 0; i < dimension; ++i) {
      dims_[i] = size[dimension - 1 - i];
    }
    user_layout_.Reset(dimension, size, strides);
    layout_.Reset(primitive, type);
    convert_in_.Reset(dnnConversionCreate<T>, user_layout_, layout_);
    convert_out_.Reset(dnnConversionCreate<T>, layout_, user_layout_);
    share_mem_ =
        share_mem_if_possible && dnnLayoutCompare(layout_, user_layout_);
    if (!share_mem_) {
      // If we do not do copy, we will create the buffer and own it.
      void* allocated = nullptr;
      MKLDNN_SAFE_CALL(dnnAllocateBuffer<T>(&allocated, layout_));
      buffer_.reset(allocated, [](void* ptr) -> void {
        MKLDNN_CHECK(dnnReleaseBuffer<T>(ptr));
      });
    }
  }

  // Initialize an MKLMemory, with the size and stride
  // derived from the tensor itself.
  MKLMemory(
      const TensorCPU& tensor,
      const dnnPrimitive_t primitive,
      const dnnResourceType_t type,
      bool share_mem_if_possible = false) {
    dims_ = tensor.dims();
    size_t dimension = tensor.ndim();
    size_t size[dimension];
    size_t strides[dimension];
    for (int i = 0; i < dimension; ++i) {
      size[i] = tensor.dim(dimension - i - 1);
      strides[i] = (i == 0) ? 1 : strides[i - 1] * size[i - 1];
    }
    user_layout_.Reset(tensor.ndim(), size, strides);
    layout_.Reset(primitive, type);
    convert_in_.Reset(dnnConversionCreate<T>, user_layout_, layout_);
    convert_out_.Reset(dnnConversionCreate<T>, layout_, user_layout_);
    share_mem_ =
        share_mem_if_possible && dnnLayoutCompare<T>(layout_, user_layout_);
    if (!share_mem_) {
      // If we do not do copy, we will create the buffer and own it.
      void* allocated = nullptr;
      MKLDNN_SAFE_CALL(dnnAllocateBuffer<T>(&allocated, layout_));
      buffer_.reset(allocated, [](void* ptr) -> void {
        MKLDNN_CHECK(dnnReleaseBuffer<T>(ptr));
      });
    }
  }

  // Destructs the MKLMemory.
  ~MKLMemory() {}

  void CopyFrom(const void* ptr) {
    if (share_mem_) {
      buffer_.reset(const_cast<void*>(ptr), [](void*) -> void {});
    } else {
      MKLDNN_SAFE_CALL(dnnConversionExecute<T>(
          convert_in_, const_cast<void*>(ptr), buffer_.get()));
    }
  }

  void CopyFrom(const TensorCPU& tensor) {
    CAFFE_ENFORCE_EQ(
        tensor.dims(),
        dims_,
        "Dims does not match the expected dims of the resource.");
    CopyFrom(tensor.template data<T>());
  }

  bool ShareFrom(const void* ptr) {
    if (share_mem_) {
      buffer_.reset(const_cast<void*>(ptr), [](void*) -> void {});
      return true;
    } else {
      return false;
    }
  }

  bool ShareFrom(const TensorCPU& tensor) {
    CAFFE_ENFORCE_EQ(
        tensor.dims(),
        dims_,
        "Dims does not match the expected dims of the resource.");
    return ShareFrom(tensor.template data<T>());
  }

  void CopyTo(void* ptr) {
    if (buffer_.get() == ptr) {
      // This is already mapping to the same memory region. Skip copy.
      return;
    }
    CAFFE_ENFORCE(
        buffer_.get(), "Canot copy out from an empty internal resource.");
    MKLDNN_SAFE_CALL(dnnConversionExecute<T>(convert_out_, buffer_.get(), ptr));
  }

  void CopyTo(TensorCPU* tensor) {
    if (buffer_.get() == tensor->mutable_data<T>()) {
      // This is already mapping to the same memory region. Skip copy.
      return;
    }
    tensor->Resize(dims_);
    CopyTo(tensor->mutable_data<T>());
  }

  inline void* buffer() {
    return buffer_.get();
  }

  inline const void* buffer() const {
    return buffer_.get();
  }

  // Returns a view of the content. We mark this function const, but be noted
  // that the returned std::shared_ptr is not const protected - user discretion
  // is recommended for correctness.
  std::shared_ptr<void> View(dnnLayout_t layout_wanted) const {
    if (dnnLayoutCompare(layout_wanted, layout_)) {
      // If they are the same, return the original content.
      return std::shared_ptr<void>(buffer_);
    } else {
      void* temp_buffer;
      MKLDNN_SAFE_CALL(dnnAllocateBuffer<T>(&temp_buffer, layout_wanted));
      PrimitiveWrapper<T> convert(
          dnnConversionCreate<T>, layout_, layout_wanted);
      MKLDNN_SAFE_CALL(dnnConversionExecute<T>(convert, buffer_, temp_buffer));
      return std::shared_ptr<void>(temp_buffer, [](void* buffer) -> void {
        MKLDNN_CHECK(dnnReleaseBuffer<T>(buffer));
      });
    }
  }

 private:
  bool share_mem_;
  // The internal buffer in the specific dnn layout.
  std::shared_ptr<void> buffer_;
  // The dimensions in the same order as Caffe2 does. This is used to
  // interface with C2.
  vector<TIndex> dims_;
  // The user dnn layout.
  LayoutWrapper<T> user_layout_;
  // The internal dnn layout.
  LayoutWrapper<T> layout_;
  // The primitive to use to convert from user layout to internal layout
  PrimitiveWrapper<T> convert_in_;
  // The primitive to use to convert from internal layout to user layout
  PrimitiveWrapper<T> convert_out_;

  DISABLE_COPY_AND_ASSIGN(MKLMemory);
};

} // namespace mkl
} // namespace caffe2

#endif // CAFFE2_UTILS_MKL_MKL_MEMORY_HPP_
