#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "caffe2/core/operator.h"
#include "caffe2/core/tensor.h"
#include "caffe2/utils/string_utils.h"

namespace caffe2 {
namespace {

const char kDatasetFieldSeparator = ':';
const char* kDatasetLengthField = "lengths";

// how much percent to grow the dataset when needed
const int kDatasetGrowthPct = 40;

// used for lengths tensors in the dataset
using TLength = int32_t;
// used for all internal dataset operations (offsets, sizes to read, etc.)
using TOffset = int64_t;

/**
 * Provides functionality to iterate across a list of tensors where some
 * of those tensors represent lengths in a hierarchical structure.
 */
class TreeIterator {
 public:
  struct FieldDesc {
    int id;
    int lengthFieldId = -1;
    std::string name;
  };

  explicit TreeIterator(const std::vector<std::string>& fields) {
    // populate field vector and split field names
    fields_.resize(fields.size());
    std::vector<std::vector<std::string>> nameParts(fields_.size());
    for (int i = 0; i < fields.size(); ++i) {
      auto& field = fields_.at(i);
      field.name = fields[i];
      field.id = i;
      field.lengthFieldId = -1;
      nameParts.at(i) = split(kDatasetFieldSeparator, field.name);
    }

    // populate lengthFields
    for (const auto& field : fields_) {
      const auto& parts = nameParts.at(field.id);
      if (!parts.empty() && parts.back() == kDatasetLengthField) {
        lengthFieldIds_.push_back(field.id);
      }
    }

    // find length-field with maximum prefix matching for each field
    for (auto& field : fields_) {
      // by default, we are matching against the root domain
      int maxMatchLevel = 1;
      int maxMatchLengthFieldId = -1;
      for (int j = 0; j < numLengthFields(); ++j) {
        const auto& lenField = lengthField(j);
        // a length field can't have itself as its length field
        if (field.id == lenField.id) {
          continue;
        }
        auto lf = nameParts.at(lenField.id);
        auto lfEnd = lf.end() - 1;
        // check whether this lengthField is a prefix for this field name
        if (std::mismatch(lf.begin(), lfEnd, nameParts.at(field.id).begin())
                .first != lfEnd) {
          continue;
        }
        if (lf.size() > maxMatchLevel) {
          maxMatchLevel = lf.size();
          maxMatchLengthFieldId = j;
        }
      }
      field.lengthFieldId = maxMatchLengthFieldId;
    }

    // check that fields are topologically sorted
    // (no length field depends on a length defined afterwards)
    for (const auto& field : fields_) {
      const auto* lengthField = lengthFieldFor(field);
      CAFFE_ENFORCE(
          (lengthField == nullptr) || (lengthField->id < field.id),
          "Error: Field ",
          field.id,
          " (",
          field.name,
          ") ",
          "depends on a field defined afterwards: ",
          lengthField->id,
          " (",
          lengthField->name,
          ").");
    }
  }

  void advance(
      const std::vector<const TLength*>& lengths,
      std::vector<TOffset>& offsets,
      std::vector<TOffset>& sizes,
      std::vector<TOffset>& limits,
      TOffset num) {
    std::vector<TOffset> newOffsets;
    CHECK_EQ(lengths.size(), numLengthFields());
    CHECK_EQ(offsets.size(), numOffsetFields());
    sizes.resize(offsets.size());
    newOffsets.resize(offsets.size());
    // first index, top level
    {
      auto limit = limits[0];
      auto offset = offsets[0];
      CAFFE_ENFORCE(limit >= offset, "Tried to advance past end of cursor.");
      TOffset total = std::min(limit - offset, num);
      sizes[0] = total;
      newOffsets[0] = offset + total;
    }
    // child indices
    for (int j = 1; j < numOffsetFields(); ++j) {
      TOffset total = 0;
      int parentOffsetId = offsetFieldIdFor(lengthField(j - 1));
      const TLength* length = lengths[j - 1] + offsets[parentOffsetId];
      for (int k = 0; k < sizes[parentOffsetId]; ++k) {
        total += *(length++);
      }
      auto offset = offsets[j];
      CAFFE_ENFORCE(
          offset + total <= limits[j],
          "Inconsistent field length: ",
          "tried to advance past the end of field ",
          j);
      sizes[j] = total;
      newOffsets[j] = offset + total;
    }
    offsets = newOffsets;
  }

  // Corresponds to the number of fields that have "length" as its last name
  int numLengthFields() const {
    return lengthFieldIds_.size();
  }

  // Corresponds to the number of length fields + 1 (for the top-level domain)
  int numOffsetFields() const {
    return numLengthFields() + 1;
  }

  // Get lengthField description for the given field
  const FieldDesc* lengthFieldFor(const FieldDesc& desc) {
    return (desc.lengthFieldId == -1)
        ? nullptr
        : &fields_.at(lengthFieldIds_.at(desc.lengthFieldId));
  }

  // Get lengthField description for the given lengthFieldId, where
  // 0 <= lengthFieldId < numLengthFields()
  const FieldDesc& lengthField(int lengthFieldId) {
    return fields_.at(lengthFieldIds_.at(lengthFieldId));
  }

  // Returns the index into the 'offset' vector for the given field.
  int offsetFieldIdFor(const FieldDesc& fieldDesc) {
    return fieldDesc.lengthFieldId + 1;
  }

  // Returns the field description for all fields.
  const std::vector<FieldDesc>& fields() {
    return fields_;
  }

 private:
  // Description of each field
  std::vector<FieldDesc> fields_;
  // Index into fields_ above for the fields that are lengths.
  std::vector<int> lengthFieldIds_;
};

class TreeCursor {
 public:
  explicit TreeCursor(const TreeIterator& iterator) : it(iterator) {}
  std::vector<TOffset> offsets;
  std::mutex mutex_;
  TreeIterator it;
};

class CreateTreeCursorOp : public Operator<CPUContext> {
 public:
  CreateTreeCursorOp(const OperatorDef& operator_def, Workspace* ws)
      : Operator(operator_def, ws),
        fields_(OperatorBase::GetRepeatedArgument<std::string>("fields")) {}

  bool RunOnDevice() override {
    *OperatorBase::Output<std::unique_ptr<TreeCursor>>(0) =
        std::unique_ptr<TreeCursor>(new TreeCursor(TreeIterator(fields_)));
    return true;
  }

 private:
  std::vector<std::string> fields_;
};

class ResetCursorOp : public Operator<CPUContext> {
 public:
  ResetCursorOp(const OperatorDef& operator_def, Workspace* ws)
      : Operator(operator_def, ws) {}

  bool RunOnDevice() override {
    auto& cursor = OperatorBase::Input<std::unique_ptr<TreeCursor>>(0);
    std::lock_guard<std::mutex> lock(cursor->mutex_);
    cursor->offsets.clear();
    return true;
  }
};

class CheckDatasetConsistencyOp : public Operator<CPUContext> {
 public:
  CheckDatasetConsistencyOp(const OperatorDef& operator_def, Workspace* ws)
      : Operator(operator_def, ws),
        iterator_(OperatorBase::GetRepeatedArgument<std::string>("fields")) {}

  bool RunOnDevice() override {
    std::vector<const TLength*> lengths;
    std::vector<TOffset> limits;
    std::vector<TOffset> sizes;
    std::vector<TOffset> offsets;
    CAFFE_ENFORCE(
        InputSize() == iterator_.fields().size(),
        "Invalid number of fields. Expected ",
        iterator_.fields().size(),
        ", got ",
        InputSize());
    sizes.resize(iterator_.numOffsetFields());
    // gather length data
    lengths.resize(iterator_.numLengthFields());
    for (int i = 0; i < lengths.size(); ++i) {
      lengths[i] = Input(iterator_.lengthField(i).id).data<TLength>();
    }
    // gather size limits
    limits.assign(sizes.size(), std::numeric_limits<TOffset>::max());
    for (int i = 0; i < iterator_.fields().size(); ++i) {
      int lengthIdx = iterator_.fields()[i].lengthFieldId + 1;
      TOffset size = (TOffset)Input(i).dims()[0];
      if (limits[lengthIdx] == std::numeric_limits<TOffset>::max()) {
        limits[lengthIdx] = size;
      } else {
        CAFFE_ENFORCE(
            limits[lengthIdx] == size,
            "Inconsistent sizes for fields belonging to same domain.",
            " Field: ",
            i,
            " (",
            iterator_.fields()[i].name,
            "); Length field index: ",
            lengthIdx,
            "); Previous size: ",
            limits[lengthIdx],
            "; New size: ",
            size);
      }
    }
    // advance to the end
    offsets.assign(sizes.size(), 0);
    iterator_.advance(lengths, offsets, sizes, limits, limits[0]);
    for (int i = 0; i < limits.size(); ++i) {
      CAFFE_ENFORCE(limits[i] == offsets[i]);
    }
    return true;
  }

 private:
  TreeIterator iterator_;
};

class ReadNextBatchOp : public Operator<CPUContext> {
 public:
  ReadNextBatchOp(const OperatorDef& operator_def, Workspace* ws)
      : Operator(operator_def, ws),
        batchSize_(OperatorBase::GetSingleArgument<int>("batch_size", 1)) {}

  bool RunOnDevice() override {
    auto& cursor = OperatorBase::Input<std::unique_ptr<TreeCursor>>(0);
    CAFFE_ENFORCE(InputSize() == cursor->it.fields().size() + 1);
    std::vector<const TLength*> lengths;
    std::vector<TOffset> limits;
    std::vector<TOffset> sizes;
    std::vector<TOffset> offsets;
    TLength lenZero = 0;
    sizes.resize(cursor->it.numOffsetFields());
    // gather length data
    lengths.resize(cursor->it.numLengthFields());
    for (int i = 0; i < lengths.size(); ++i) {
      auto& a = Input(cursor->it.lengthField(i).id + 1);
      if (a.size() > 0) {
        lengths[i] = a.data<int>();
      } else {
        lengths[i] = &lenZero;
      }
    }
    // gather size limits
    limits.assign(sizes.size(), std::numeric_limits<TOffset>::max());
    for (int i = 0; i < cursor->it.fields().size(); ++i) {
      int lengthFieldIdx = cursor->it.fields()[i].lengthFieldId + 1;
      limits[lengthFieldIdx] =
          std::min(limits[lengthFieldIdx], (TOffset)Input(i + 1).dims()[0]);
    }
    // advance cursor
    {
      std::lock_guard<std::mutex> lock(cursor->mutex_);
      if (cursor->offsets.empty()) {
        cursor->offsets.assign(sizes.size(), 0);
      }
      offsets = cursor->offsets;
      cursor->it.advance(lengths, cursor->offsets, sizes, limits, batchSize_);
    }
    // gather data
    std::vector<TIndex> outDim;
    for (int i = 0; i < cursor->it.fields().size(); ++i) {
      auto lengthIdx = cursor->it.fields()[i].lengthFieldId + 1;
      auto size = sizes[lengthIdx];
      auto offset = offsets[lengthIdx];
      auto& in = Input(i + 1);
      auto innerSize = in.size_from_dim(1);
      outDim = in.dims();
      outDim[0] = size;
      auto* out = Output(i);
      out->Resize(outDim);
      if (out->size() == 0) {
        continue;
      }
      void* src =
          (char*)in.raw_data() + offset * innerSize * in.meta().itemsize();
      void* dst = out->raw_mutable_data(in.meta());
      context_.template CopyItems<CPUContext, CPUContext>(
          in.meta(), out->size(), src, dst);
    }
    return true;
  }
  int batchSize_;
};

class ComputeOffsetOp : public Operator<CPUContext> {
 public:
  ComputeOffsetOp(const OperatorDef& operator_def, Workspace* ws)
      : Operator(operator_def, ws) {}

  bool RunOnDevice() override {
    auto& cursor = OperatorBase::Input<std::unique_ptr<TreeCursor>>(0);
    CAFFE_ENFORCE(InputSize() == cursor->it.fields().size() + 1);
    auto* out = Output(0);
    std::vector<const TLength*> lengths;
    std::vector<TOffset> limits;
    std::vector<TOffset> sizes;
    std::vector<TOffset> offsets;
    TLength lenZero = 0;
    sizes.resize(cursor->it.numOffsetFields());
    // gather length data
    lengths.resize(cursor->it.numLengthFields());
    for (int i = 0; i < lengths.size(); ++i) {
      auto& a = Input(cursor->it.lengthField(i).id + 1);
      if (a.size() > 0) {
        lengths[i] = a.data<int>();
      } else {
        lengths[i] = &lenZero;
      }
    }
    // gather size limits
    limits.assign(sizes.size(), std::numeric_limits<TOffset>::max());
    for (int i = 0; i < cursor->it.fields().size(); ++i) {
      int lengthFieldIdx = cursor->it.fields()[i].lengthFieldId + 1;
      limits[lengthFieldIdx] =
          std::min(limits[lengthFieldIdx], (TOffset)Input(i + 1).dims()[0]);
    }
    out->Resize(limits.at(0) + 1, sizes.size());
    auto* out_data = out->mutable_data<int64_t>();
    for (int k = 0; k <= limits.at(0); k++) {
      // advance cursor
      if (cursor->offsets.empty()) {
        cursor->offsets.assign(sizes.size(), 0);
      }
      // write output
      std::copy(cursor->offsets.begin(), cursor->offsets.end(), out_data);
      out_data += sizes.size();
      cursor->it.advance(lengths, cursor->offsets, sizes, limits, 1);
    }
    cursor->offsets.assign(sizes.size(), 0); // reSet after getting meta info
    return true;
  }
};

class SortAndShuffleOp : public Operator<CPUContext> {
 public:
  SortAndShuffleOp(const OperatorDef& operator_def, Workspace* ws)
      : Operator(operator_def, ws),
        sort_by_field_idx_(
            OperatorBase::GetSingleArgument<int>("sort_by_field_idx", 1)),
        batch_size_(OperatorBase::GetSingleArgument<int>("batch_size", 1)),
        shuffle_size_(OperatorBase::GetSingleArgument<int>("shuffle_size", 1)) {
  }

  bool RunOnDevice() override {
    auto& cursor = OperatorBase::Input<std::unique_ptr<TreeCursor>>(0);
    CAFFE_ENFORCE(InputSize() == cursor->it.fields().size() + 1);
    CAFFE_ENFORCE(
        -1 <= sort_by_field_idx_ &&
        sort_by_field_idx_ < cursor->it.fields().size());

    int size;
    if (sort_by_field_idx_ != -1) {
      size = Input(sort_by_field_idx_ + 1).dims()[0];
    } else {
      size = Input(1).dims()[0];
    }

    CAFFE_ENFORCE(
        batch_size_ > 0 && shuffle_size_ > 0 &&
        0 < batch_size_ * shuffle_size_ && batch_size_ * shuffle_size_ <= size);
    int num_batch = size / batch_size_;

    auto* out = Output(0);
    out->Resize(size);
    auto* out_data = out->mutable_data<int64_t>();

    vector<int> shuffle_idx(size);
    iota(shuffle_idx.begin(), shuffle_idx.end(), 0);

    if (sort_by_field_idx_ != -1) {
      auto& sortblob = Input(sort_by_field_idx_ + 1);
      auto* sortdata = sortblob.data<int>();
      // must sort by a field at the root level
      CAFFE_ENFORCE(
          cursor->it.fields()[sort_by_field_idx_].lengthFieldId == -1);
      sort(shuffle_idx.begin(), shuffle_idx.end(), [&sortdata](int i1, int i2) {
        return sortdata[i1] < sortdata[i2];
      });
    }

    if (batch_size_ * shuffle_size_ > 1) {
      int offset = 0;
      while (offset + batch_size_ * shuffle_size_ < size) {
        std::shuffle(
            shuffle_idx.begin() + offset,
            shuffle_idx.begin() + offset + batch_size_ * shuffle_size_,
            std::default_random_engine());
        offset += batch_size_ * shuffle_size_;
      }
    }

    vector<int> batch_idx(num_batch);
    iota(batch_idx.begin(), batch_idx.end(), 0);
    std::shuffle(
        batch_idx.begin(), batch_idx.end(), std::default_random_engine());

    for (int i = 0; i < num_batch; i++) {
      std::copy(
          shuffle_idx.begin() + batch_idx[i] * batch_size_,
          shuffle_idx.begin() + (batch_idx[i] + 1) * batch_size_,
          out_data);
      out_data += batch_size_;
    }
    std::copy(
        shuffle_idx.begin() + num_batch * batch_size_,
        shuffle_idx.end(),
        out_data);

    return true;
  }

  int sort_by_field_idx_;
  int batch_size_;
  int shuffle_size_;
};

class ReadRandomBatchOp : public Operator<CPUContext> {
 public:
  ReadRandomBatchOp(const OperatorDef& operator_def, Workspace* ws)
      : Operator(operator_def, ws),
        batchSize_(OperatorBase::GetSingleArgument<int>("batch_size", 1)) {}
  bool RunOnDevice() override {
    auto& cursor = OperatorBase::Input<std::unique_ptr<TreeCursor>>(0);
    auto& idxblob = Input(1);
    auto& offsetsmat = Input(2);
    CAFFE_ENFORCE(InputSize() == cursor->it.fields().size() + 3);
    auto idxvec = idxblob.template data<int64_t>();
    auto& offsetdim = offsetsmat.dims();
    // gather data
    std::vector<TIndex> outDim;
    int64_t idx;
    {
      std::lock_guard<std::mutex> lock(cursor->mutex_);
      cursor->offsets.resize(1);
      idx = cursor->offsets.at(0);
      cursor->offsets.at(0) += batchSize_;
    }

    for (int i = 0; i < cursor->it.fields().size(); ++i) {
      auto lengthIdx = cursor->it.fields()[i].lengthFieldId + 1;
      auto& in = Input(i + 3);
      outDim = in.dims();
      outDim.at(0) = 0;
      auto idxbegin = idx;
      for (int j = 0; j < batchSize_; ++j) {
        if (idx >= idxblob.size()) {
          break;
        }
        CAFFE_ENFORCE(
            (idxvec[idx] + 1) * offsetdim[1] + lengthIdx < offsetsmat.size(),
            "Out of bound when trying to get elem from offsetsmat");
        auto offsetptr = offsetsmat.template data<TOffset>() +
            idxvec[idx] * offsetdim[1] + lengthIdx;
        auto offset = *offsetptr;
        auto size = *(offsetptr + offsetdim[1]) - offset;
        outDim.at(0) += size; // accumulate over the batch
        idx++;
      }
      idx = idxbegin; // reSet
      auto* out = Output(i);
      out->Resize(outDim);
      if (out->size() == 0) {
        continue;
      }
      auto dst = static_cast<char*>(out->raw_mutable_data(in.meta()));
      int block_size = in.size() / in.dim(0);
      auto block_bytesize = in.size_from_dim(1) * in.meta().itemsize();
      CAFFE_ENFORCE(
          block_bytesize == in.nbytes() / in.dim(0),
          "block_bytesize should be consistent with data dim");
      auto src_base = static_cast<const char*>(in.raw_data());
      int start = 0;
      for (int j = 0; j < batchSize_; ++j) {
        if (idx >= idxblob.size()) {
          break;
        }
        auto offsetptr = offsetsmat.template data<TOffset>() +
            idxvec[idx] * offsetdim[1] + lengthIdx;
        auto offset = *offsetptr;
        auto size = *(offsetptr + offsetdim[1]) - offset;
        // copy data
        auto src = src_base + offset * block_bytesize;
        context_.template CopyItems<CPUContext, CPUContext>(
            in.meta(), size * block_size, src, dst + start * block_bytesize);
        start += size;
        idx++;
      }
      idx = idxbegin; // reSet
    }
    return true;
  }
  int batchSize_;
};

template <class Context>
class AppendOp final : public Operator<Context> {
 public:
  USE_OPERATOR_CONTEXT_FUNCTIONS;
  AppendOp(const OperatorDef& operator_def, Workspace* ws)
      : Operator<Context>(operator_def, ws) {}

  bool RunOnDevice() override {
    auto& a = Input(0);
    auto& b = Input(1);
    auto* c = Output(0);
    CAFFE_ENFORCE(b.ndim() >= 1);
    if (a.size() == 0) {
      c->CopyFrom(b);
      return true;
    }
    CAFFE_ENFORCE(&a == c, "First argument must be in-place.");
    CAFFE_ENFORCE(c->ndim() == b.ndim());
    CAFFE_ENFORCE(b.ndim() == c->ndim());
    CAFFE_ENFORCE(a.meta() == b.meta());
    for (int i = 1; i < a.ndim(); ++i) {
      CAFFE_ENFORCE(a.dims()[i] == b.dims()[i]);
    }
    auto oldSize = c->size();
    c->Extend(b.dims()[0], kDatasetGrowthPct, &context_);
    auto* dst = (char*)c->raw_mutable_data() + oldSize * b.meta().itemsize();
    context_.template CopyItems<Context, Context>(
        b.meta(), b.size(), b.raw_data(), dst);
    return true;
  }
};

template <class Context>
class AtomicAppendOp final : public Operator<Context> {
 public:
  USE_OPERATOR_CONTEXT_FUNCTIONS;
  AtomicAppendOp(const OperatorDef& operator_def, Workspace* ws)
      : Operator<Context>(operator_def, ws) {}

  bool RunOnDevice() override {
    auto& mutex = OperatorBase::Input<std::unique_ptr<std::mutex>>(0);
    const auto numFields = (InputSize() - 1) / 2;
    CAFFE_ENFORCE(OutputSize() == numFields);

    std::lock_guard<std::mutex> guard(*mutex);

    // 1: checks
    for (int i = 0; i < numFields; ++i) {
      auto& a = Input(1 + i);
      auto& b = Input(1 + i + numFields);
      auto* c = Output(i);
      CAFFE_ENFORCE(b.ndim() >= 1);
      if (a.size() == 0) {
        continue;
      }
      CAFFE_ENFORCE(
          (void*)&a == (void*)c, "Appended-to arguments must be in-place.");
      CAFFE_ENFORCE(c->ndim() == b.ndim());
      CAFFE_ENFORCE(b.ndim() == c->ndim());
      CAFFE_ENFORCE(a.meta() == b.meta());
      for (int j = 1; j < a.ndim(); ++j) {
        CAFFE_ENFORCE(a.dims()[j] == b.dims()[j]);
      }
    }

    // 2: copies
    for (int i = 0; i < numFields; ++i) {
      auto& a = Input(1 + i);
      auto& b = Input(1 + i + numFields);
      auto* c = Output(i);
      if (a.size() == 0) {
        c->CopyFrom(b);
        continue;
      }
      auto oldSize = c->size();
      c->Extend(b.dims()[0], kDatasetGrowthPct, &context_);
      auto* dst = (char*)c->raw_mutable_data() + oldSize * b.meta().itemsize();
      context_.template CopyItems<Context, Context>(
          b.meta(), b.size(), b.raw_data(), dst);
    }
    return true;
  }
};

REGISTER_CPU_OPERATOR(CreateTreeCursor, CreateTreeCursorOp);
REGISTER_CPU_OPERATOR(ResetCursor, ResetCursorOp);
REGISTER_CPU_OPERATOR(ReadNextBatch, ReadNextBatchOp);
REGISTER_CPU_OPERATOR(ComputeOffset, ComputeOffsetOp);
REGISTER_CPU_OPERATOR(SortAndShuffle, SortAndShuffleOp);
REGISTER_CPU_OPERATOR(ReadRandomBatch, ReadRandomBatchOp);
REGISTER_CPU_OPERATOR(CheckDatasetConsistency, CheckDatasetConsistencyOp);
REGISTER_CPU_OPERATOR(Append, AppendOp<CPUContext>);
REGISTER_CPU_OPERATOR(AtomicAppend, AtomicAppendOp<CPUContext>);

OPERATOR_SCHEMA(CreateTreeCursor)
    .NumInputs(0)
    .NumOutputs(1)
    .SetDoc(R"DOC(
Creates a cursor to iterate through a list of tensors, where some of those
tensors contains the lengths in a nested schema. The schema is determined by
the `fields` arguments.

For example, to represent the following schema:

  Struct(
      a=Int(),
      b=List(List(Int),
      c=List(
          Struct(
             c1=String,
             c2=List(Int),
          ),
      ),
  )

the field list will be:
  [
      "a",
      "b:lengths",
      "b:values:lengths",
      "b:values:values",
      "c:lengths",
      "c:c1",
      "c:c2:lengths",
      "c:c2:values",
  ]

And for the following instance of the struct:

  Struct(
      a=3,
      b=[[4, 5], [6, 7, 8], [], [9]],
      c=[
          Struct(c1='alex', c2=[10, 11]),
          Struct(c1='bob', c2=[12]),
      ],
  )

The values of the fields will be:
  {
      "a": [3],
      "b:lengths": [4],
      "b:values:lengths": [2, 3, 0, 1],
      "b:values:values": [4, 5, 6, 7, 8, 9],
      "c:lengths": [2],
      "c:c1": ["alex", "bob"],
      "c:c2:lengths": [2, 1],
      "c:c2:values", [10, 11, 12],
  }

In general, every field name in the format "{prefix}:lengths" defines a domain
"{prefix}", and every subsequent field in the format "{prefx}:{field}" will
be in that domain, and the length of the domain is provided for each entry of
the parent domain. In the example, "b:lengths" defines a domain of length 4, so
every field under domain "b" will have 4 entries.
The "lengths" field for a given domain must appear before any reference to
that domain.

Returns a pointer to an instance of the Cursor, which keeps the current offset
on each of the domains defined by `fields`. Cursor also ensures thread-safety
such that ReadNextBatch and ResetCursor can be used safely in parallel.

A cursor does not contain data per se, so calls to ReadNextBatch actually need
to pass a list of blobs containing the data to read for each one of the fields.
)DOC")
    .Output(0, "cursor", "A blob pointing to an instance of a new TreeCursor.")
    .Arg(
        "fields",
        "A list of strings each one representing a field of the dataset.");

OPERATOR_SCHEMA(ResetCursor)
    .NumInputs(1)
    .NumOutputs(0)
    .SetDoc(R"DOC(
Resets the offsets for the given TreeCursor. This operation is thread safe.
)DOC")
    .Input(0, "cursor", "A blob containing a pointer to the cursor.");

OPERATOR_SCHEMA(ReadNextBatch)
    .NumInputs(1, INT_MAX)
    .NumOutputs(1, INT_MAX)
    .SetDoc(R"DOC(
Read the next batch of examples out of the given cursor and data blobs.

Input(0) is a blob pointing to a TreeCursor, and
[Input(1),... Input(num_fields)] a list of tensors containing the data for
each field of the dataset.

ReadNextBatch is thread safe.
)DOC")
    .Input(0, "cursor", "A blob containing a pointer to the cursor.")
    .Input(1, "dataset_field_0", "First dataset field")
    .Output(0, "field_0", "Tensor containing the next batch for field 0.")
    .Arg("batch_size", "Number of top-level entries to read.");

OPERATOR_SCHEMA(ComputeOffset)
    .NumInputs(1, INT_MAX)
    .NumOutputs(1)
    .SetDoc(R"DOC(
Compute the offsets matrix given cursor and data blobs. Need to be ran at
beginning or after reseting cursor

Input(0) is a blob pointing to a TreeCursor, and
[Input(1),... Input(num_fields)] a list of tensors containing the data for
each field of the dataset.

ComputeOffset is thread safe.
)DOC")
    .Input(0, "cursor", "A blob containing a pointer to the cursor.")
    .Input(1, "dataset_field_0", "First dataset field")
    .Output(0, "field_0", "Tensor containing offset info for this chunk.");

OPERATOR_SCHEMA(SortAndShuffle)
    .NumInputs(1, INT_MAX)
    .NumOutputs(1)
    .SetDoc(R"DOC(
Compute the sorted indices given a field index to sort by and break the sorted
indices into chunks of shuffle_size * batch_size and shuffle each chunk,
finally we shuffle between batches. If sort_by_field_idx is -1 we skip sort.

For example, we have data sorted as
1,2,3,4,5,6,7,8,9,10,11,12

and batchSize = 2 and shuffleSize = 3, when we shuffle we get:
[3,1,4,6,5,2] [12,10,11,8,9,7]

After this we will shuffle among different batches with size 2
[3,1],[4,6],[5,2],[12,10],[11,8],[9,7]

We may end up with something like
[9,7],[5,2],[12,10],[4,6],[3,1],[11,8]

Input(0) is a blob pointing to a TreeCursor, and
[Input(1),... Input(num_fields)] a list of tensors containing the data for
each field of the dataset.

SortAndShuffle is thread safe.
)DOC")
    .Input(0, "cursor", "A blob containing a pointer to the cursor.")
    .Input(1, "dataset_field_0", "First dataset field")
    .Output(0, "indices", "Tensor containing sorted indices.");

OPERATOR_SCHEMA(ReadRandomBatch)
    .NumInputs(1, INT_MAX)
    .NumOutputs(1, INT_MAX)
    .SetDoc(R"DOC(
Read the next batch of examples out of the given cursor,
idx blob, offset matrix and data blobs.

Input(0) is a blob pointing to a TreeCursor,
Input(1) is a blob pointing to the shuffled idx
Input(2) is a blob pointing to the offset matrix and
[Input(3),... Input(num_fields)] a list of tensors containing the data for
each field of the dataset.

ReadRandomBatch is thread safe.
)DOC")
    .Input(0, "cursor", "A blob containing a pointer to the cursor.")
    .Input(1, "idx", "idx with a shuffled order.")
    .Input(2, "offsetsmat", "offset matrix containing length offset info.")
    .Input(3, "dataset_field_0", "First dataset field")
    .Output(0, "field_0", "Tensor containing the next batch for field 0.")
    .Arg("batch_size", "Number of top-level entries to read.");

OPERATOR_SCHEMA(CheckDatasetConsistency)
    .NumInputs(1, INT_MAX)
    .NumOutputs(0)
    .SetDoc(R"DOC(
Checks that the given data fields represents a consistent dataset unther
the schema specified by the `fields` argument. Operator fails if the fields
are not consistent. If data is consistent, each field's data can be safely
appended to an existing dataset, keeping it consistent.
)DOC")
    .Input(0, "field_0", "Data for field 0.")
    .Arg(
        "fields",
        "List of strings representing the string names in the format"
        "specified in the doc for CreateTreeCursor.");

OPERATOR_SCHEMA(Append)
    .NumInputs(2)
    .NumOutputs(1)
    .EnforceInplace({{0, 0}})
    .SetDoc(R"DOC(
Append input 2 to the end of input 1.
Input 1 must be the same as output, that is, it is required to be in-place.
Input 1 may have to be re-allocated in order for accommodate to the new size.
Currently, an exponential growth ratio is used in order to ensure amortized
constant time complexity.
All except the outer-most dimension must be the same between input 1 and 2.
)DOC")
    .Input(0, "dataset", "The tensor to be appended to.")
    .Input(1, "new_data", "Tensor to append to the end of dataset.")
    .Output(0, "dataset", "Same as input 0, representing the mutated tensor.");

OPERATOR_SCHEMA(AtomicAppend)
    .NumInputs(3, INT_MAX)
    .NumOutputs(1, INT_MAX)
    .AllowInplace([](int in, int out) { return in == out + 1; });

SHOULD_NOT_DO_GRADIENT(CreateTreeCursor);
SHOULD_NOT_DO_GRADIENT(ResetCursor);
SHOULD_NOT_DO_GRADIENT(ReadNextBatch);
SHOULD_NOT_DO_GRADIENT(ComputeOffset);
SHOULD_NOT_DO_GRADIENT(ReadRandomBatch);
SHOULD_NOT_DO_GRADIENT(CheckDatasetConsistency);
SHOULD_NOT_DO_GRADIENT(Append);
SHOULD_NOT_DO_GRADIENT(AtomicAppend);
}
}
