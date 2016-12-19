#include "store_handler.h"

#include <memory>

#include "caffe2/core/typeid.h"

namespace caffe2 {

StoreHandler::~StoreHandler() {
  // NOP; definition is here to make sure library contains
  // symbols for this abstract class.
}

CAFFE_KNOWN_TYPE(std::unique_ptr<StoreHandler>);

} // namespace caffe2
