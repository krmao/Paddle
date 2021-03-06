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
#include "paddle/fluid/framework/lod_rank_table.h"
#include "paddle/fluid/framework/lod_tensor_array.h"
#include "paddle/fluid/framework/op_registry.h"
#include "paddle/fluid/operators/detail/safe_ref.h"
#include "paddle/fluid/platform/device_context.h"

namespace paddle {
namespace operators {

struct CopyRange {
  size_t begin;
  size_t end;
};

class LoDTensorToArrayOp : public framework::OperatorBase {
 public:
  LoDTensorToArrayOp(const std::string &type,
                     const framework::VariableNameMap &inputs,
                     const framework::VariableNameMap &outputs,
                     const framework::AttributeMap &attrs)
      : OperatorBase(type, inputs, outputs, attrs) {}

 private:
  void RunImpl(const framework::Scope &scope,
               const platform::Place &place) const override {
    auto &x = detail::Ref(scope.FindVar(Input("X")), "Cannot find input %s",
                          Input("X"))
                  .Get<framework::LoDTensor>();
    auto &rank_table = detail::Ref(scope.FindVar(Input("RankTable")))
                           .Get<framework::LoDRankTable>();
    auto &out = *detail::Ref(scope.FindVar(Output("Out")))
                     .GetMutable<framework::LoDTensorArray>();
    auto &items = rank_table.items();
    auto max_seq_len = items[0].length;
    auto rank_level = rank_table.level();

    PADDLE_ENFORCE_LT(rank_level, x.lod().size(),
                      "Input should be a LOD tensor, and size is at least %d",
                      rank_level + 1);
    out.resize(max_seq_len);
    std::vector<std::vector<CopyRange>> copy_ranges(max_seq_len);

    // set out[i] lod
    for (size_t t = 0; t < max_seq_len; t++) {
      auto &lod = *out[t].mutable_lod();
      lod.clear();
      for (auto &item : items) {
        if (t >= item.length) {
          break;
        }
        size_t start_idx = x.lod()[rank_level][item.index] + t;
        auto lod_and_offset = framework::GetSubLoDAndAbsoluteOffset(
            x.lod(), start_idx, start_idx + 1, rank_level + 1);
        auto &lod_length = lod_and_offset.first;
        framework::AppendLoD(&lod, lod_length);
        size_t start_offset = lod_and_offset.second.first;
        size_t end_offset = lod_and_offset.second.second;
        copy_ranges[t].emplace_back(CopyRange{start_offset, end_offset});
      }
    }
    for (size_t i = 0; i < max_seq_len; ++i) {
      auto &ranges = copy_ranges[i];
      size_t height = std::accumulate(
          ranges.begin(), ranges.end(), 0UL,
          [](size_t a, const CopyRange &b) { return a + b.end - b.begin; });
      auto x_dim = x.dims();
      x_dim[0] = static_cast<int64_t>(height);
      out[i].Resize(x_dim);
      out[i].mutable_data(x.place(), x.type());
      size_t offset = 0;
      for (auto &each_range : ranges) {
        size_t len = each_range.end - each_range.begin;
        if (len == 0) {
          continue;
        }
        // out[i][offset: offset+len] = x[each_range.begin: each_range.end]
        auto slice = out[i].Slice(static_cast<int>(offset),
                                  static_cast<int>(offset + len));

        platform::DeviceContextPool &pool =
            platform::DeviceContextPool::Instance();
        auto &dev_ctx = *pool.Get(place);

        framework::Copy(x.Slice(static_cast<int>(each_range.begin),
                                static_cast<int>(each_range.end)),
                        x.place(), dev_ctx, &slice);
        offset += len;
      }
    }
  }
};

class LoDTensorToArrayOpProtoMaker : public framework::OpProtoAndCheckerMaker {
 public:
  LoDTensorToArrayOpProtoMaker(OpProto *proto, OpAttrChecker *op_checker)
      : OpProtoAndCheckerMaker(proto, op_checker) {
    AddInput("X", "");
    AddInput("RankTable", "");
    AddOutput("Out", "");
    AddComment("");
  }
};

class LoDTensorToArrayInferShape : public framework::InferShapeBase {
 public:
  void operator()(framework::InferShapeContext *context) const override {
    PADDLE_ENFORCE(context->HasInput("X"),
                   "Input(X) of LoDTensorToArrayOp should not be null.");
    PADDLE_ENFORCE(
        context->HasInput("RankTable"),
        "Input(RankTable) of LoDTensorToArrayOp should not be null.");

    PADDLE_ENFORCE(context->HasOutput("Out"),
                   "Output(Out) of LoDTensorToArrayOp should not be null.");

    auto x_dim = context->GetInputDim("X");
    // The first dim of each LoDTensor in Output can only be set at run-time.;
    // We still have to Resize each LoDTensor in Output.
    context->SetOutputDim("Out", x_dim);
  }
};

class LoDTensorToArrayInferVarType : public framework::VarTypeInference {
 public:
  void operator()(const framework::OpDesc &op_desc,
                  framework::BlockDesc *block) const override {
    for (auto &out_var : op_desc.Output("Out")) {
      block->Var(out_var)->SetType(framework::proto::VarDesc::LOD_TENSOR_ARRAY);
    }
  }
};

class LoDTensorToArrayGradMaker : public framework::SingleGradOpDescMaker {
 public:
  using framework::SingleGradOpDescMaker::SingleGradOpDescMaker;

 protected:
  std::unique_ptr<framework::OpDesc> Apply() const override {
    auto *grad_op = new framework::OpDesc();
    grad_op->SetType("array_to_lod_tensor");
    grad_op->SetInput("X", OutputGrad("Out"));
    grad_op->SetInput("RankTable", Input("RankTable"));
    grad_op->SetOutput("Out", InputGrad("X"));
    grad_op->SetAttrMap(Attrs());
    return std::unique_ptr<framework::OpDesc>(grad_op);
  }
};

}  // namespace operators
}  // namespace paddle

namespace ops = paddle::operators;
REGISTER_OPERATOR(lod_tensor_to_array, ops::LoDTensorToArrayOp,
                  ops::LoDTensorToArrayOpProtoMaker,
                  ops::LoDTensorToArrayInferShape,
                  ops::LoDTensorToArrayInferVarType,
                  ops::LoDTensorToArrayGradMaker);
