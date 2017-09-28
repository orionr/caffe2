/**
 * Copyright (c) 2016-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>
#include <vector>

#include "caffe2/operators/string_ops.h"

namespace caffe2 {

class StringJoinOpTest : public testing::Test {
 public:
  bool runOp(const TensorCPU& input) {
    auto* blob = ws_.CreateBlob("X");
    auto* tensor = blob->GetMutable<TensorCPU>();
    tensor->ResizeLike(input);
    tensor->ShareData(input);

    OperatorDef def;
    def.set_name("test");
    def.set_type("StringJoin");
    def.add_input("X");
    def.add_output("Y");

    auto op = CreateOperator(def, &ws_);
    return op->Run();
  }

  const std::string* checkAndGetOutput(int outputSize) {
    const auto* output = ws_.GetBlob("Y");
    EXPECT_NE(output, nullptr);
    EXPECT_TRUE(output->IsType<TensorCPU>());
    const auto& outputTensor = output->Get<TensorCPU>();
    EXPECT_EQ(outputTensor.ndim(), 1);
    EXPECT_EQ(outputTensor.dim(0), outputSize);
    EXPECT_EQ(outputTensor.size(), outputSize);
    return outputTensor.data<std::string>();
  }

 protected:
  Workspace ws_;
};

TEST_F(StringJoinOpTest, testString1DJoin) {
  std::vector<std::string> input = {"a", "xx", "c"};

  auto blob = caffe2::make_unique<Blob>();
  auto* tensor = blob->GetMutable<TensorCPU>();
  tensor->Resize(input.size());
  auto* data = tensor->mutable_data<std::string>();
  for (int i = 0; i < input.size(); ++i) {
    *data++ = input[i];
  }

  EXPECT_TRUE(runOp(*tensor));

  const auto* outputData = checkAndGetOutput(input.size());
  EXPECT_EQ(outputData[0], "a,");
  EXPECT_EQ(outputData[1], "xx,");
  EXPECT_EQ(outputData[2], "c,");
}

TEST_F(StringJoinOpTest, testString2DJoin) {
  std::vector<std::vector<std::string>> input = {{"aa", "bb", "cc"},
                                                 {"dd", "ee", "ff"}};

  auto blob = caffe2::make_unique<Blob>();
  auto* tensor = blob->GetMutable<TensorCPU>();
  tensor->Resize(input.size(), input[0].size());
  auto* data = tensor->mutable_data<std::string>();
  for (int i = 0; i < input.size(); ++i) {
    for (int j = 0; j < input[0].size(); ++j) {
      *data++ = input[i][j];
    }
  }

  EXPECT_TRUE(runOp(*tensor));

  const auto* outputData = checkAndGetOutput(input.size());
  EXPECT_EQ(outputData[0], "aa,bb,cc,");
  EXPECT_EQ(outputData[1], "dd,ee,ff,");
}

TEST_F(StringJoinOpTest, testFloat1DJoin) {
  std::vector<float> input = {3.90, 5.234, 8.12};

  auto blob = caffe2::make_unique<Blob>();
  auto* tensor = blob->GetMutable<TensorCPU>();
  tensor->Resize(input.size());
  auto* data = tensor->mutable_data<float>();
  for (int i = 0; i < input.size(); ++i) {
    *data++ = input[i];
  }

  EXPECT_TRUE(runOp(*tensor));

  const auto* outputData = checkAndGetOutput(input.size());
  EXPECT_EQ(outputData[0], "3.9,");
  EXPECT_EQ(outputData[1], "5.234,");
  EXPECT_EQ(outputData[2], "8.12,");
}

TEST_F(StringJoinOpTest, testFloat2DJoin) {
  std::vector<std::vector<float>> input = {{1.23, 2.45, 3.56},
                                           {4.67, 5.90, 6.32}};

  auto blob = caffe2::make_unique<Blob>();
  auto* tensor = blob->GetMutable<TensorCPU>();
  tensor->Resize(input.size(), input[0].size());
  auto* data = tensor->mutable_data<float>();
  for (int i = 0; i < input.size(); ++i) {
    for (int j = 0; j < input[0].size(); ++j) {
      *data++ = input[i][j];
    }
  }

  EXPECT_TRUE(runOp(*tensor));

  const auto* outputData = checkAndGetOutput(input.size());
  EXPECT_EQ(outputData[0], "1.23,2.45,3.56,");
  EXPECT_EQ(outputData[1], "4.67,5.9,6.32,");
}

TEST_F(StringJoinOpTest, testLong2DJoin) {
  std::vector<std::vector<int64_t>> input = {{100, 200}, {1000, 2000}};

  auto blob = caffe2::make_unique<Blob>();
  auto* tensor = blob->GetMutable<TensorCPU>();
  tensor->Resize(input.size(), input[0].size());
  auto* data = tensor->mutable_data<int64_t>();
  for (int i = 0; i < input.size(); ++i) {
    for (int j = 0; j < input[0].size(); ++j) {
      *data++ = input[i][j];
    }
  }

  EXPECT_TRUE(runOp(*tensor));

  const auto* outputData = checkAndGetOutput(input.size());
  EXPECT_EQ(outputData[0], "100,200,");
  EXPECT_EQ(outputData[1], "1000,2000,");
}
}
