/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 *  Copyright (c) 2017 by Contributors
 * \file codegen_cuda.cc
 */
#include <tvm/base.h>
#include <tvm/runtime/registry.h>
#include <tvm/packed_func_ext.h>
#include <cmath>
#include <vector>
#include <string>
#include "codegen_cuda.h"

namespace tvm {
namespace codegen {

CodeGenCUDA::CodeGenCUDA() {
  restrict_keyword_ = "__restrict__";
}

void CodeGenCUDA::Init(bool output_ssa) {
  CodeGenC::Init(output_ssa);
  vid_global_barrier_state_ = GetUniqueName(runtime::symbol::tvm_global_barrier_state);
  vid_global_barrier_expect_ = GetUniqueName("__barrier_expect");
  CHECK_EQ(vid_global_barrier_state_, runtime::symbol::tvm_global_barrier_state);
}

void CodeGenCUDA::AddFunction(LoweredFunc f) {
  this->stream << "extern \"C\" __global__ ";
  CodeGenC::AddFunction(f);
}

std::string CodeGenCUDA::Finish() {
  if (enable_fp16_) {
    decl_stream << "#include <cuda_fp16.h>\n";
    decl_stream << "__device__ half max"
                << "(half a, half b)\n"
                << "{\n  return __hgt(__half(a), __half(b)) ? a : b;\n}\n";
    decl_stream << "__device__ half min(half a, half b)\n"
                << "{\n  return __hlt(__half(a), __half(b)) ? a : b;\n}\n";
    decl_stream << "__device__ half operator<="
                << "(__half a,  __half b)\n"
                << "{\n  return __hlt(a, b);\n}\n";
    decl_stream << "__device__ half operator+"
                << "(__half a,  __half &b)\n"
                <<"{\n  return __hadd(a, b);\n}\n";
    decl_stream << "__device__ half operator*"
                << "(__half a, __half b)\n"
                <<   "{\n  return __hmul(a, b);\n}\n";
  }

  if (enable_int8_) {
    decl_stream << "#include <sm_61_intrinsics.h>\n";
  }

  if (need_math_constants_h_) {
    decl_stream << "#include <math_constants.h>\n";
  }

  if (need_mma_h_) {
    decl_stream << "#include <mma.h>\n";
  }

  return CodeGenC::Finish();
}

void CodeGenCUDA::VisitStmt_(const ir::For* op) {
  CHECK(is_const_int(op->min, 0));
  if (op->for_type == ir::ForType::Unrolled) {
    PrintIndent();
    stream << "#pragma unroll\n";
  }
  CodeGenC::VisitStmt_(op);
}

void CodeGenCUDA::BindThreadIndex(const IterVar& iv) {
  CHECK(!var_idmap_.count(iv->var.get()));
  var_idmap_[iv->var.get()] =
      CastFromTo(iv->thread_tag, UInt(32), iv->var.type());
}

void CodeGenCUDA::PrintType(Type t, std::ostream& os) {  // NOLINT(*)
  int lanes = t.lanes();
  if (t.is_handle()) {
    CHECK_EQ(lanes, 1)
        << "do not yet support vector types";
    os << "void*"; return;
  }
  bool fail = false;
  if (t.is_float()) {
    switch (t.bits()) {
      case 16:
        enable_fp16_ = true;
        if (lanes == 1) {
          os << "half";
        } else if (lanes <= 8) {
          CHECK_EQ(lanes % 2, 0) << "only support even lane for half type";
          os << "float" << lanes / 2;
        } else {
          fail = true;
        }
        break;
      case 32: os << "float"; break;
      case 64: os << "double"; break;
      default: fail = true; break;
    }
    if (!fail && (lanes == 1 || t.bits() == 16)) return;
    if (!fail && (lanes >= 2 && lanes <= 4)) {
      os << lanes; return;
    }
  } else if (t == Bool()) {
    os << "bool"; return;
  } else if (t.is_uint() || t.is_int()) {
    if (t.is_uint()) {
      if (t.lanes() != 1) {
        os << "u";
      } else {
        os << "unsigned ";
      }
    }
    switch (t.bits()) {
      case 8: {
        if (t.lanes() == 4) {
          // directly 4 8 bit int in integer.
          enable_int8_ = true;

          // We use int for int8x4 instead of char4 because using char4 is
          // likely to produce extra instructions to pack four int8 elements
          // into 32-bit data.
          os << "int"; return;
        } else if (t.lanes() == 8) {
          enable_int8_ = true;
          os << "int2"; return;
        } else if (t.lanes() == 16) {
          enable_int8_ = true;
          os << "int4"; return;
        } else if (!t.is_uint() && t.lanes() == 1) {
          os << "signed char"; break;
        } else {
          os << "char"; break;
        }
      }
      case 16: os << "short"; break;
      case 32: os << "int"; break;
      case 64: {
        if (sizeof(long) != 8) { // NOLINT(*)
          if (t.lanes() == 1) {
            os << "long long"; break;
          } else if (t.lanes() == 2) {
            os << "longlong"; break;
          } else {
            // No longlong3, longlong4
            LOG(FATAL) << "Cannot convert type " << t << " to CUDA type on a L32 platform";
          }
        } else {
          os << "long"; break;
        }
      }
      case 1: os << "int"; break;
      default: fail = true; break;
    }
    if (!fail && lanes == 1) {
      return;
    }
    if (!fail && (lanes >= 2 && lanes <= 4)) {
      os << lanes; return;
    }
  }
  LOG(FATAL) << "Cannot convert type " << t << " to CUDA type";
}

void CodeGenCUDA::PrintVecBinaryOp(
    const std::string&op, Type t,
    Expr lhs, Expr rhs, std::ostream& os) {  // NOLINT(*)
  // unpacking operations.
  int lanes = t.lanes();

  {
    // The assignment below introduces side-effect, and the resulting value cannot
    // be reused across multiple expression, thus a new scope is needed
    int vec_scope = BeginScope();

    // default: unpack into individual ops.
    std::string vlhs = SSAGetID(PrintExpr(lhs), lhs.type());
    std::string vrhs = SSAGetID(PrintExpr(rhs), rhs.type());
    std::string sret = GetUniqueName("_");
    {
      // delcare type.
      this->PrintIndent();
      this->PrintType(t, stream);
      stream << ' ' << sret << ";\n";
    }
    for (int i = 0; i < lanes; ++i) {
      std::ostringstream value_temp;
      if (isalpha(op[0])) {
        value_temp << op << "(";
        PrintVecElemLoad(vlhs, lhs.type(), i, value_temp);
        value_temp << ", ";
        PrintVecElemLoad(vrhs, rhs.type(), i, value_temp);
        value_temp << ")";
      } else {
        value_temp << "(";
        PrintVecElemLoad(vlhs, lhs.type(), i, value_temp);
        value_temp << op;
        PrintVecElemLoad(vrhs, rhs.type(), i, value_temp);
        value_temp << ")";
      }
      PrintVecElemStore(sret, t, i, value_temp.str());
    }
    os << sret;
    EndScope(vec_scope);
  }
}

void CodeGenCUDA::PrintVecElemLoad(
    const std::string& vec, Type t, int i, std::ostream& os) {  // NOLINT(*)
  static const char access[] = {'x', 'y', 'z', 'w'};
  CHECK(i >= 0 && i < 4);
  if (t.is_int() && t.bits() == 8) {
    os << "(0x000000ff & (" << vec << " >> " << i * 8 << "))";
  } else {
    os << vec << "." << access[i];
  }
}

void CodeGenCUDA::PrintVecElemStore(
    const std::string& vec, Type t, int i, const std::string& value) {
  this->PrintIndent();
  static const char access[] = {'x', 'y', 'z', 'w'};
  CHECK(i >= 0 && i < 4);
  if (t.is_int() && t.bits() == 8) {
    stream << vec << "=" << vec << " & ~(0x000000ff << " << i * 8 << ") | ("
        << value << " << " << i * 8 << ");\n";
  } else {
    stream << vec << "." << access[i] << " = " << value << ";\n";
  }
}

void CodeGenCUDA::PrintStorageSync(const Call* op) {
  const std::string& sync = op->args[0].as<StringImm>()->value;
  if (sync == "warp") {
    // DO nothing.
  } else if (sync == "shared") {
    this->PrintIndent();
    this->stream << "__syncthreads();\n";
  } else if (sync == "global") {
    if (!need_global_barrier_) {
      need_global_barrier_ = true;
      this->decl_stream << "extern \"C\" __device__ unsigned "
                        << vid_global_barrier_state_ << ";\n";
    }
    // global synchronizer
    std::string is_load = PrintExpr(op->args[1]);
    std::string num_blocks = PrintExpr(op->args[2]);
    this->PrintIndent();
    // In theory only threadfence is needed
    // but we observed problems with only threadfence
    this->stream <<"__threadfence_system();\n";
    this->PrintIndent();
    this->stream <<"if (" << is_load << ") {\n";
    int wb = this->BeginScope();
    this->PrintIndent();
    this->stream << "atomicAdd(&" << vid_global_barrier_state_ << ", 1);\n";
    this->PrintIndent();
    std::string ptr = GetUniqueName("pf");
    this->stream << "volatile unsigned* "
                 << ptr << " = &" << vid_global_barrier_state_<< ";\n";
    this->PrintIndent();
    this->stream << vid_global_barrier_expect_ << " += " << num_blocks << ";\n";
    this->PrintIndent();
    this->stream <<"while (" << ptr << "[0] < " << vid_global_barrier_expect_ << ");\n";
    this->EndScope(wb);
    this->PrintIndent();
    this->stream <<"}\n";
    this->PrintIndent();
    this->stream <<"__syncthreads();\n";
  }
}

void CodeGenCUDA::PrintStorageScope(
    const std::string& scope, std::ostream& os) { // NOLINT(*)
  CHECK_NE(scope, "global");
  if (scope == "shared") {
    os << "__shared__";
  }
}

void CodeGenCUDA::VisitExpr_(const Call *op, std::ostream& os) {
  if (op->is_intrinsic(intrinsic::tvm_fill_fragment)) {
    need_mma_h_ = true;
    CHECK_EQ(op->args.size(), 6U);
    os << "nvcuda::wmma::fill_fragment(";
    this->PrintExpr(op->args[0], os);
    os << "[";
    this->PrintExpr(op->args[4], os);
    os << "], ";
    this->PrintExpr(op->args[5], os);
    os << ")";
  } else if (op->is_intrinsic(intrinsic::tvm_load_matrix_sync)) {
    need_mma_h_ = true;
    CHECK_EQ(op->args.size(), 8U);
    os << "nvcuda::wmma::load_matrix_sync(";
    this->PrintExpr(op->args[0], os);
    os << "[";
    this->PrintExpr(op->args[4], os);
    os << "], ";
    this->PrintExpr(op->args[5], os);
    os << ", ";
    this->PrintExpr(op->args[6], os);
    os << ")";
  } else if (op->is_intrinsic(intrinsic::tvm_store_matrix_sync)) {
    need_mma_h_ = true;
    CHECK_EQ(op->args.size(), 8U);
    os << "nvcuda::wmma::store_matrix_sync(";
    this->PrintExpr(op->args[5], os);
    os << ", ";
    this->PrintExpr(op->args[0], os);
    os << "[";
    this->PrintExpr(op->args[4], os);
    os << "], ";
    this->PrintExpr(op->args[6], os);
    if (const StringImm *str = op->args[7].as<StringImm>()) {
      os << ", nvcuda::wmma::mem_" << str->value;
    } else {
      LOG(FATAL) << "Invalid parameters";
    }
    os << ")";
  } else if (op->is_intrinsic(intrinsic::tvm_mma_sync)) {
    need_mma_h_ = true;
    CHECK_EQ(op->args.size(), 8U);
    os << "nvcuda::wmma::mma_sync(";
    for (int i = 0; i < 4; ++i) {
      this->PrintExpr(op->args[i * 2], os);
      os << "[";
      this->PrintExpr(op->args[i * 2 + 1], os);
      os << "]" << ((i < 3) ? ", ": ")");
    }
  } else {
    CodeGenC::VisitExpr_(op, os);
  }
}

void CodeGenCUDA::VisitStmt_(const AttrStmt* op) {
  if (op->attr_key == attr::fragment_shape) {
    const Variable* buffer = op->node.as<Variable>();
    const StringImm* shape_str = op->value.as<StringImm>();
    fragment_shapes[buffer] = shape_str->value;
  } else if (op->attr_key == attr::fragment_layout) {
    const Variable* buffer = op->node.as<Variable>();
    const StringImm* layout_str = op->value.as<StringImm>();
    fragment_layouts[buffer] = layout_str->value;
  }
  CodeGenC::VisitStmt_(op);
}

void CodeGenCUDA::VisitStmt_(const Allocate* op) {
  CHECK(!is_zero(op->condition));
  std::string vid = AllocVarID(op->buffer_var.get());
  if (op->new_expr.defined()) {
    // Prefer global static allocation for the program
    CHECK_EQ(op->free_function, "nop");
    std::string new_data = PrintExpr(op->new_expr);
    this->PrintIndent();
    PrintType(op->type, stream);
    stream << "* "<< vid << '=' << new_data << ";\n";
  } else {
    this->PrintIndent();
    int32_t constant_size = op->constant_allocation_size();
    CHECK_GT(constant_size, 0)
      << "Can only handle constant size stack allocation for now";
    const Variable* buffer = op->buffer_var.as<Variable>();
    std::string scope = alloc_storage_scope_.at(buffer);
    if (scope.find("wmma.") == 0) {
      if (scope == "wmma.matrix_a" || scope == "wmma.matrix_b") {
        CHECK(op->type == Float(16) || op->type == Int(8) || op->type == UInt(8))
          << "Matrix_a and matrix_b only support half or char or unsigned char type for now";
      } else {
        CHECK(op->type == Float(16) || op->type == Float(32) || op->type == Int(32))
          << "Accumulator only support half, float and int type for now";
      }
      constant_size = GetWmmaFragmentSize(scope, buffer, constant_size);
      PrintWmmaScope(scope, op->type, buffer, stream);
    } else {
      PrintStorageScope(scope, stream);
      stream << ' ';
      PrintType(op->type, stream);
    }
    stream << ' '<< vid << '['
           << constant_size << "];\n";
  }
  RegisterHandleType(op->buffer_var.get(), op->type);
  this->PrintStmt(op->body);
}

void CodeGenCUDA::VisitStmt_(const Evaluate *op) {
  if (is_const(op->value)) return;
  const Call* call = op->value.as<Call>();
  if (call && call->is_intrinsic(intrinsic::tvm_global_barrier_kinit)) {
    PrintIndent();
    stream << "__shared__ unsigned " << vid_global_barrier_expect_ << ";\n";
    PrintIndent();
    stream << "if (threadIdx.x == 0) {\n";
    PrintIndent();
    stream << "  " << vid_global_barrier_expect_ << " = 0;\n";
    PrintIndent();
    stream << "}\n";
  } else {
    CodeGenC::VisitStmt_(op);
  }
}

void CodeGenCUDA::VisitExpr_(const Ramp* op, std::ostream& os) {
  os << "((make_int" << op->lanes << ")(";
  for (int i = 0; i < op->lanes; i++) {
    os << "(" << PrintExpr(op->base) << ")" << "+(" << PrintExpr(op->stride) << "*" << i <<")";
    if (i != op->lanes - 1)
      os << ", ";
  }
  os << "))";
}

void CodeGenCUDA::VisitExpr_(const Broadcast* op, std::ostream& os) {   // NOLINT(*)
  if (op->type.is_int() && op->type.bits() == 8 && op->lanes == 4) {
    // make_int8x4
    const int64_t *p = as_const_int(op->value);
    CHECK(p);
    int64_t v = *p & 0xFF;
    v = (v << 24) | (v << 16) | (v << 8) | v;
    os << "(int)" << v;
    return;
  }

  std::string v = PrintExpr(op->value);
  os << "make_";
  PrintType(op->type, os);
  os << '(';
  for (int i = 0; i < op->lanes; ++i) {
    if (i != 0) os << ", ";
    os << v;
  }
  os << ')';
}

void CodeGenCUDA::VisitExpr_(const Shuffle* op, std::ostream &os) {
  std::vector<std::string> to_shuffle(op->vectors.size());
  for (int i = 0, e = op->vectors.size(); i < e; ++i) {
    CHECK(op->vectors[i].type().lanes() == 1) << "Only scalars can be shuffled in CUDA!";
    to_shuffle[i] = PrintExpr(op->vectors[i]);
  }
  os << "make_";
  PrintType(op->type, os);
  os << '(';
  for (int i = 0, e = op->indices.size(); i < e; ++i) {
    const int64_t *val = as_const_int(op->indices[i]);
    CHECK(val && *val >= 0 && (int) *val < (int) to_shuffle.size());
    if (i != 0) os << ", ";
    os << to_shuffle[*val];
  }
  os << ')';
}

inline void PrintConst(const FloatImm* op, std::ostream& os, CodeGenCUDA* p) { // NOLINT(*)
  switch (op->type.bits()) {
    case 64: case 32: {
      std::ostringstream temp;
      if (std::isinf(op->value)) {
        if (op->value < 0) {
          temp << "-";
        }
        temp << ((op->type.bits() == 32) ? "CUDART_INF_F" : "CUDART_INF");
        p->need_math_constants_h_ = true;
      } else if (std::isnan(op->value)) {
        temp << ((op->type.bits() == 32) ? "CUDART_NAN_F" : "CUDART_NAN");
        p->need_math_constants_h_ = true;
      } else {
        temp << std::scientific << op->value;
        if (op->type.bits() == 32) temp << 'f';
      }
      p->MarkConst(temp.str());
      os << temp.str();
      break;
    }
    case 16: {
      os << "__float2half_rn";
      os << '(' << std::scientific << op->value << 'f' << ')';
      break;
    }
    default: LOG(FATAL) << "Bad bit-width for float: " << op->type << "\n";
  }
}


void CodeGenCUDA::VisitExpr_(const FloatImm *op, std::ostream& os) { // NOLINT(*)
  PrintConst(op, os, this);
}

void CodeGenCUDA::PrintWmmaScope(const std::string &scope, Type t,
    const Variable* variable, std::ostream &os) {
  std::stringstream type;
  PrintType(t, type);
  std::string shape_str = fragment_shapes[variable];
  if (scope == "wmma.matrix_a") {
    need_mma_h_ = true;
    std::string layout_str = fragment_layouts[variable];
    os << "nvcuda::wmma::fragment<nvcuda::wmma::matrix_a, "
      << shape_str << ", " << type.str() << ", nvcuda::wmma::" << layout_str <<">";
  } else if (scope == "wmma.matrix_b") {
    need_mma_h_ = true;
    std::string layout_str = fragment_layouts[variable];
    os << "nvcuda::wmma::fragment<nvcuda::wmma::matrix_b, "
       << shape_str << ", " << type.str() << ", nvcuda::wmma::" << layout_str <<">";
  } else if (scope == "wmma.accumulator") {
    need_mma_h_ = true;
    os << "nvcuda::wmma::fragment<nvcuda::wmma::accumulator, "
       << shape_str << ", "<< type.str() << ">";
  }
}

int32_t CodeGenCUDA::GetWmmaFragmentSize(const std::string &scope,
                                         const Variable* variable, int32_t size) {
  std::string shape_str = fragment_shapes[variable];
  size_t m, n, k;
  size_t last_pos = 0, pos = 0;
  pos = shape_str.find(", ", last_pos);
  m = std::stoi(shape_str.substr(last_pos, pos - last_pos));
  last_pos = pos + 2;
  pos = shape_str.find(", ", last_pos);
  n = std::stoi(shape_str.substr(last_pos, pos - last_pos));
  last_pos = pos + 2;
  k = std::stoi(shape_str.substr(last_pos, shape_str.length() - last_pos));
  if (scope == "wmma.matrix_a") {
    return size / m / k;
  } else if (scope == "wmma.matrix_b") {
    return size / n / k;
  } else if (scope == "wmma.accumulator") {
    return size / m / n;
  }
  return 0;
}

}  // namespace codegen
}  // namespace tvm
