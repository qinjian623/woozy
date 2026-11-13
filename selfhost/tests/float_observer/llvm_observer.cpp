// LLVM-only floating-point observation helper for the Wz driver's pre-opt IR.
//
// This program does not include or link any language front end, semantic
// checker, Native emitter, helper executable, or runtime source compiler.  It
// parses exactly the driver-produced pre-optimization LLVM module, validates
// the driver metadata against the actual LLVM function types, replaces the
// entry body with typed calls, and writes the observation module.
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <json-c/json.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

namespace {
using namespace llvm;
namespace fs = std::filesystem;

struct BadInput : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct FunctionMeta {
  std::string source;
  std::string llvm;
  std::string result;
  std::vector<std::string> parameters;
  bool covered = false;
};

std::string read_text(const fs::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw BadInput("cannot read " + path.string());
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

void write_text(const fs::path &path, const std::string &text) {
  fs::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(text.data(), std::streamsize(text.size()));
  if (!file)
    throw BadInput("cannot write " + path.string());
}

std::string hex_hash(const std::string &text) {
  auto hash = SHA256::hash(ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(text.data()), text.size()));
  static const char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (uint8_t byte : hash) {
    out.push_back(digits[byte >> 4]);
    out.push_back(digits[byte & 15]);
  }
  return out;
}

std::string json_string(json_object *object, const char *name) {
  json_object *value = nullptr;
  if (!json_object_object_get_ex(object, name, &value) ||
      !json_object_is_type(value, json_type_string))
    throw BadInput(std::string("metadata field is not a string: ") + name);
  return json_object_get_string(value);
}

bool json_bool(json_object *object, const char *name) {
  json_object *value = nullptr;
  if (!json_object_object_get_ex(object, name, &value) ||
      !json_object_is_type(value, json_type_boolean))
    throw BadInput(std::string("metadata field is not a bool: ") + name);
  return json_object_get_boolean(value) != 0;
}

std::string metadata_preopt_hash(const fs::path &path) {
  json_object *root = json_object_from_file(path.c_str());
  if (!root)
    throw BadInput("cannot parse metadata " + path.string());
  std::unique_ptr<json_object, decltype(&json_object_put)> guard(root,
                                                                 json_object_put);
  return json_string(root, "preopt_llvm_sha256");
}

using MetadataMap = std::map<std::string, std::vector<FunctionMeta>>;

MetadataMap load_metadata(const fs::path &path) {
  json_object *root = json_object_from_file(path.c_str());
  if (!root)
    throw BadInput("cannot parse metadata " + path.string());
  std::unique_ptr<json_object, decltype(&json_object_put)> guard(root,
                                                                 json_object_put);
  if (json_string(root, "backend") != "native")
    throw BadInput("metadata backend is not native");
  json_object *functions = nullptr;
  if (!json_object_object_get_ex(root, "functions", &functions) ||
      !json_object_is_type(functions, json_type_array))
    throw BadInput("metadata functions array is missing");
  MetadataMap result;
  size_t count = json_object_array_length(functions);
  for (size_t index = 0; index < count; ++index) {
    json_object *item = json_object_array_get_idx(functions, index);
    FunctionMeta meta;
    meta.source = json_string(item, "source_name");
    meta.result = json_string(item, "result");
    std::string status = json_string(item, "status");
    meta.covered = status == "covered";
    if (meta.covered)
      meta.llvm = json_string(item, "llvm");
    json_object *parameters = nullptr;
    if (!json_object_object_get_ex(item, "parameters", &parameters) ||
        !json_object_is_type(parameters, json_type_array))
      throw BadInput("metadata function parameters are missing");
    size_t parameter_count = json_object_array_length(parameters);
    for (size_t parameter = 0; parameter < parameter_count; ++parameter) {
      json_object *value = json_object_array_get_idx(parameters, parameter);
      if (!json_object_is_type(value, json_type_string))
        throw BadInput("metadata parameter is not a string");
      meta.parameters.emplace_back(json_object_get_string(value));
    }
    result[meta.source].push_back(std::move(meta));
  }
  return result;
}

unsigned result_width(const std::string &type) {
  if (type == "f32")
    return 32;
  if (type == "f64")
    return 64;
  if (type == "bool")
    return 1;
  return 0;
}

bool actual_return_matches(Type *type, const std::string &name) {
  if (name == "f32")
    return type->isFloatTy();
  if (name == "f64")
    return type->isDoubleTy();
  if (name == "bool")
    return type->isIntegerTy(1);
  return false;
}

bool actual_parameter_matches(Type *type, const std::string &name) {
  if (name == "f32")
    return type->isFloatTy();
  if (name == "f64")
    return type->isDoubleTy();
  return false;
}

struct Request {
  std::string source;
  std::string result;
  std::vector<std::string> bits;
};

std::vector<Request> load_requests(const fs::path &path) {
  std::ifstream file(path);
  if (!file)
    throw BadInput("cannot read " + path.string());
  std::vector<Request> requests;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty())
      continue;
    std::istringstream row(line);
    Request request;
    if (!(row >> request.source >> request.result))
      throw BadInput("malformed request line");
    std::string bits;
    while (row >> bits)
      request.bits.push_back(bits);
    requests.push_back(std::move(request));
  }
  return requests;
}

void verify_math(Module &module, const char *label) {
  for (Function &function : module) {
    if (function.getName().starts_with("llvm.fma") ||
        function.getName().starts_with("llvm.fmuladd"))
      throw BadInput(std::string(label) + ": unexpected FMA");
    for (BasicBlock &block : function)
      for (Instruction &instruction : block)
        if (isa<FPMathOperator>(instruction) &&
            instruction.getFastMathFlags().any())
          throw BadInput(std::string(label) + ": fast/contract flag");
  }
}

std::string write_metadata(const MetadataMap &metadata,
                           size_t requests, const std::string &input_hash,
                           const std::string &observe_hash) {
  json_object *root = json_object_new_object();
  std::unique_ptr<json_object, decltype(&json_object_put)> guard(root,
                                                                 json_object_put);
  json_object_object_add(root, "status", json_object_new_string("passed"));
  json_object_object_add(root, "requests",
                         json_object_new_uint64(requests));
  json_object_object_add(root, "input_sha256",
                         json_object_new_string(input_hash.c_str()));
  json_object_object_add(root, "observe_sha256",
                         json_object_new_string(observe_hash.c_str()));
  json_object_object_add(root, "module_verified_before",
                         json_object_new_boolean(1));
  json_object_object_add(root, "module_verified_after",
                         json_object_new_boolean(1));
  json_object *functions = json_object_new_array();
  for (const auto &[name, entries] : metadata) {
    for (const auto &meta : entries) {
      json_object *item = json_object_new_object();
      json_object_object_add(item, "source_name",
                             json_object_new_string(name.c_str()));
      json_object_object_add(item, "llvm",
                             json_object_new_string(meta.llvm.c_str()));
      json_object_object_add(item, "result",
                             json_object_new_string(meta.result.c_str()));
      json_object *parameters = json_object_new_array();
      for (const auto &parameter : meta.parameters)
        json_object_array_add(parameters,
                              json_object_new_string(parameter.c_str()));
      json_object_object_add(item, "parameters", parameters);
      json_object_array_add(functions, item);
    }
  }
  json_object_object_add(root, "functions", functions);
  return json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY);
}

int run(int argc, char **argv) {
  if (argc != 5)
    throw BadInput("usage: llvm_observer PREOPT.ll METADATA.json REQUESTS.tsv OUT_DIR");
  fs::path input_path = argv[1];
  fs::path metadata_path = argv[2];
  fs::path requests_path = argv[3];
  fs::path out = argv[4];
  fs::create_directories(out);
  const std::string input_text = read_text(input_path);
  const std::string input_hash = hex_hash(input_text);
  if (metadata_preopt_hash(metadata_path) != input_hash)
    throw BadInput("pre-opt LLVM hash differs from metadata");
  auto metadata = load_metadata(metadata_path);
  auto requests = load_requests(requests_path);
  if (requests.empty())
    throw BadInput("no observation requests");

  LLVMContext context;
  SMDiagnostic error;
  auto module = parseAssemblyString(input_text, error, context);
  if (!module)
    throw BadInput("cannot parse pre-opt LLVM: " + error.getMessage().str());
  if (verifyModule(*module, &errs()))
    throw BadInput("pre-opt LLVM does not verify");
  verify_math(*module, "pre-opt");

  auto *main = module->getFunction("main");
  if (!main || !main->getReturnType()->isIntegerTy(32) ||
      main->arg_size() != 0)
    throw BadInput("pre-opt module has no i32 main()");
  main->deleteBody();
  IRBuilder<> builder(BasicBlock::Create(context, "observe", main));
  auto writer = module->getOrInsertFunction(
      "write", FunctionType::get(builder.getInt64Ty(),
                                 {builder.getInt32Ty(), builder.getPtrTy(),
                                  builder.getInt64Ty()},
                                 false));
  auto *mxcsr = builder.CreateAlloca(builder.getInt32Ty());
  auto *read_mxcsr = InlineAsm::get(
      FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()}, false),
      "stmxcsr ($0)", "r,~{memory}", true);
  builder.CreateCall(read_mxcsr, {mxcsr});
  builder.CreateCall(writer,
                     {builder.getInt32(1), mxcsr, builder.getInt64(4)});

  size_t observed = 0;
  for (const Request &request : requests) {
    auto found = metadata.find(request.source);
    if (found == metadata.end())
      throw BadInput("request source is not in metadata: " + request.source);
    std::vector<const FunctionMeta *> matches;
    for (const auto &candidate : found->second) {
      if (!candidate.covered || candidate.llvm.empty())
        continue;
      if (candidate.result != request.result)
        continue;
      if (candidate.parameters.size() != request.bits.size())
        continue;
      bool parameters_match = true;
      for (size_t index = 0; index < request.bits.size(); ++index) {
        const std::string &name = candidate.parameters[index];
        unsigned width = result_width(name);
        if ((width != 32 && width != 64) ||
            request.bits[index].size() != width / 4) {
          parameters_match = false;
          break;
        }
      }
      if (parameters_match)
        matches.push_back(&candidate);
    }
    if (matches.size() != 1)
      throw BadInput("request source/signature is not uniquely covered: " +
                     request.source);
    const FunctionMeta &meta = *matches.front();
    Function *function = module->getFunction(meta.llvm);
    if (!function)
      throw BadInput("metadata LLVM symbol is absent: " + meta.llvm);
    if (!actual_return_matches(function->getReturnType(), meta.result))
      throw BadInput("actual return type differs from metadata: " +
                     request.source);
    if (function->arg_size() != meta.parameters.size())
      throw BadInput("actual argument count differs from metadata: " +
                     request.source);
    std::vector<Value *> arguments;
    for (size_t index = 0; index < request.bits.size(); ++index) {
      const std::string &type_name = meta.parameters[index];
      unsigned width = result_width(type_name);
      if (width != 32 && width != 64)
        throw BadInput("unsupported parameter type in metadata: " + type_name);
      Type *parameter_type = function->getFunctionType()->getParamType(index);
      if (!actual_parameter_matches(parameter_type, type_name))
        throw BadInput("actual parameter type differs from metadata: " +
                       request.source);
      const std::string &bits = request.bits[index];
      if (bits.size() != width / 4)
        throw BadInput("argument bit width differs from metadata: " +
                       request.source);
      auto *raw = ConstantInt::get(context, APInt(width, bits, 16));
      arguments.push_back(ConstantExpr::getBitCast(raw, parameter_type));
    }
    auto *actual = builder.CreateCall(function->getFunctionType(), function,
                                      arguments);
    Value *raw = nullptr;
    unsigned width = result_width(meta.result);
    if (width == 1)
      raw = builder.CreateZExt(actual, builder.getInt8Ty());
    else if (width == 32 || width == 64)
      raw = builder.CreateBitCast(actual, builder.getIntNTy(width));
    else
      throw BadInput("unsupported result type in metadata: " + meta.result);
    auto *slot = builder.CreateAlloca(raw->getType());
    builder.CreateStore(raw, slot);
    builder.CreateCall(writer, {builder.getInt32(1), slot,
                                builder.getInt64(width == 1 ? 1 : width / 8)});
    ++observed;
  }
  builder.CreateRet(builder.getInt32(0));
  if (verifyModule(*module, &errs()))
    throw BadInput("observation LLVM does not verify");
  verify_math(*module, "observation");

  std::string text;
  raw_string_ostream stream(text);
  module->print(stream, nullptr);
  stream.flush();
  const std::string observe_hash = hex_hash(text);
  write_text(out / "observe.ll", text);
  write_text(out / "observer-metadata.json",
             write_metadata(metadata, observed, input_hash, observe_hash) + "\n");
  std::cout << "{\"status\":\"passed\",\"requests\":" << observed
            << ",\"input_sha256\":\"" << input_hash
            << "\",\"observe_sha256\":\"" << observe_hash << "\"}\n";
  return 0;
}
} // namespace

int main(int argc, char **argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
