/*
 * Copyright 2016, The Android Open Source Project
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

#include "ReflectionPass.h"

#include "KernelSignature.h"

#include "RSAllocationUtils.h"
#include "bcinfo/MetadataExtractor.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/SPIRV.h"

#include <map>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#define DEBUG_TYPE "rs2spirv-reflection"

using namespace llvm;

namespace {

static const StringRef CoordsNames[] = {"x", "y", "z"};
}

namespace rs2spirv {

void KernelSignature::dump() const {
  dbgs() << returnType << ' ' << name << '(' << argumentType;
  const auto CoordsNum = size_t(coordsKind);
  for (size_t i = 0; i != CoordsNum; ++i)
    dbgs() << ", " << CoordsNames[i];

  dbgs() << ")\n";
}

const std::string KernelSignature::wrapperPrefix = "%__rsov_";

namespace {

std::string TypeToString(const Type *Ty) {
  assert(Ty);
  if (Ty->isVoidTy())
    return "void";

  if (auto *IT = dyn_cast<IntegerType>(Ty)) {
    if (IT->getBitWidth() == 32)
      return "int";
    else if (IT->getBitWidth() == 8)
      return "uchar";
  }

  if (Ty->isFloatTy())
    return "float";

  if (auto *VT = dyn_cast<VectorType>(Ty)) {
    auto *ET = VT->getElementType();
    if (auto *IT = dyn_cast<IntegerType>(ET)) {
      if (IT->getBitWidth() == 32)
        return "int4";
      else if (IT->getBitWidth() == 8)
        return "uchar4";
    }
    if (ET->isFloatTy())
      return "float4";
  }

  std::string badNameString;
  raw_string_ostream badNameStream(badNameString);
  badNameStream << '[';
  Ty->print(badNameStream);
  badNameStream << ']';
  return badNameStream.str();
}

enum class RSType {
  rs_bad = -1,
  rs_void,
  rs_uchar,
  rs_int,
  rs_float,
  rs_uchar4,
  rs_int4,
  rs_float4
};

RSType StrToRsTy(StringRef S) {
  RSType Ty = StringSwitch<RSType>(S)
                  .Case("void", RSType::rs_void)
                  .Case("uchar", RSType::rs_uchar)
                  .Case("int", RSType::rs_int)
                  .Case("float", RSType::rs_float)
                  .Case("uchar4", RSType::rs_uchar4)
                  .Case("int4", RSType::rs_int4)
                  .Case("float4", RSType::rs_float4)
                  .Default(RSType::rs_bad);
  return Ty;
}

struct TypeMapping {
  RSType RSTy;
  bool isVectorTy;
  // Scalar types are accessed (loaded/stored) using wider (vector) types.
  // 'vecLen' corresponds to width of such vector type.
  // As for vector types, 'vectorWidth' is just width of such type.
  size_t vectorWidth;
  std::string SPIRVTy;
  std::string SPIRVScalarTy;
  std::string SPIRVImageFormat;
  // TODO: Handle different image formats for read and write.
  std::string SPIRVImageReadType;

  TypeMapping(RSType RSTy, bool IsVectorTy, size_t VectorLen,
              StringRef SPIRVScalarTy, StringRef SPIRVImageFormat)
      : RSTy(RSTy), isVectorTy(IsVectorTy), vectorWidth(VectorLen),
        SPIRVScalarTy(SPIRVScalarTy), SPIRVImageFormat(SPIRVImageFormat) {
    assert(vectorWidth != 0);

    if (isVectorTy) {
      std::ostringstream OSS;
      OSS << "%v" << vectorWidth << SPIRVScalarTy.drop_front().str();
      SPIRVTy = OSS.str();
      SPIRVImageReadType = SPIRVTy;
      return;
    }

    SPIRVTy = SPIRVScalarTy;
    std::ostringstream OSS;
    OSS << "%v" << vectorWidth << SPIRVScalarTy.drop_front().str();
    SPIRVImageReadType = OSS.str();
  }
};

class ReflectionPass : public ModulePass {
  typedef SmallVector<KernelSignature, 4> KernelSignatures;

  std::ostream &OS;
  bcinfo::MetadataExtractor &ME;

  static const std::map<RSType, TypeMapping> TypeMappings;

  static const TypeMapping *getMapping(RSType RsTy) {
    auto it = TypeMappings.find(RsTy);
    if (it != TypeMappings.end())
      return &it->second;

    return nullptr;
  };

  static const TypeMapping *getMapping(StringRef Str) {
    auto Ty = StrToRsTy(Str);
    return getMapping(Ty);
  }

  static const TypeMapping *getMappingOrPrintError(StringRef Str) {
    const auto *TM = ReflectionPass::getMapping(Str);
    if (!TM)
      errs() << "LLVM to SPIRV type mapping for type:\t" << Str
             << " not found\n";

    return TM;
  }

  bool emitHeader(const Module &M, const KernelSignatures &Kernel);
  bool emitDecorations(const Module &M,
                       const SmallVectorImpl<RSAllocationInfo> &RSAllocs,
                       const KernelSignatures &Kernels);
  void emitCommonTypes();
  bool extractKernelSignatures(const Module &M,
                               SmallVectorImpl<KernelSignature> &Out);
  bool emitKernelTypes(const KernelSignature &Kernel);
  bool emitInputImage(const KernelSignature &Kernel);
  void emitGLGlobalInput();
  bool emitOutputImage(const KernelSignature &Kernel);
  bool emitRSAllocImages(const SmallVectorImpl<RSAllocationInfo> &RSAllocs);
  bool emitConstants(const KernelSignature &Kernel);
  void emitRTFunctions();
  bool emitRSAllocFunctions(
      Module &M, const SmallVectorImpl<RSAllocationInfo> &RSAllocs,
      const SmallVectorImpl<RSAllocationCallInfo> &RSAllocAccesses);
  bool emitMain(const KernelSignature &Kernel,
                const SmallVectorImpl<RSAllocationInfo> &RSAllocs);

public:
  static char ID;
  explicit ReflectionPass(std::ostream &OS, bcinfo::MetadataExtractor &ME)
      : ModulePass(ID), OS(OS), ME(ME) {}

  const char *getPassName() const override { return "ReflectionPass"; }

  bool runOnModule(Module &M) override {
    DEBUG(dbgs() << "ReflectionPass\n");

    KernelSignatures Kernels;
    if (!extractKernelSignatures(M, Kernels)) {
      errs() << "Extraction of kernels failed\n";
      return false;
    }

    if (!emitHeader(M, Kernels)) {
      errs() << "Emiting header failed\n";
      return false;
    }

    SmallVector<RSAllocationInfo, 2> RSAllocs;
    if (!getRSAllocationInfo(M, RSAllocs)) {
      errs() << "Extracting rs_allocation info failed\n";
      return false;
    }

    SmallVector<RSAllocationCallInfo, 4> RSAllocAccesses;
    if (!getRSAllocAccesses(RSAllocs, RSAllocAccesses)) {
      errs() << "Extracting rsGEA/rsSEA info failed\n";
      return false;
    }

    if (!emitDecorations(M, RSAllocs, Kernels)) {
      errs() << "Emiting decorations failed\n";
      return false;
    }

    emitCommonTypes();

    for (const auto &Kernel : Kernels) {
      if (!emitKernelTypes(Kernel)) {
        errs() << "Emitting kernel types for " << Kernel.name << " failed\n";
        return false;
      }

      if (!emitInputImage(Kernel)) {
        errs() << "Emitting input image for " << Kernel.name << " failed\n";
        return false;
      }
    }

    emitGLGlobalInput();

    for (const auto &Kernel : Kernels) {
      if (!emitOutputImage(Kernel)) {
        errs() << "Emitting output image for " << Kernel.name << " failed\n";
        return false;
      }

      if (!emitRSAllocImages(RSAllocs)) {
        errs() << "Emitting rs_allocation images for " << Kernel.name
               << " failed\n";
        return false;
      }

      if (!emitConstants(Kernel)) {
        errs() << "Emitting constants for " << Kernel.name << " failed\n";
        return false;
      }
    }

    emitRTFunctions();

    for (const auto &Kernel : Kernels) {
      if (!emitRSAllocFunctions(M, RSAllocs, RSAllocAccesses)) {
        errs() << "Emitting rs_allocation runtime functions for " << Kernel.name
               << " failed\n";
        return false;
      }

      if (!emitMain(Kernel, RSAllocs)) {
        errs() << "Emitting the definition for " << Kernel.name << " failed\n";
        return false;
      }
    }

    // Return false, as the module is not modified.
    return false;
  }
};

// TODO: Add other types: bool, double, char, uchar, long, ulong
//  and their vector counterparts.
// TODO: Support vector types of width different than 4. eg. float3.
const std::map<RSType, TypeMapping> ReflectionPass::TypeMappings = {
    {RSType::rs_void, {RSType::rs_void, false, 1, "%void", ""}},
    {RSType::rs_uchar, {RSType::rs_uchar, false, 4, "%uchar", "R8ui"}},
    {RSType::rs_int, {RSType::rs_void, false, 4, "%int", "R32i"}},
    {RSType::rs_float, {RSType::rs_float, false, 4, "%float", "R32f"}},
    {RSType::rs_uchar4, {RSType::rs_uchar4, true, 4, "%uchar", "Rgba8ui"}},
    {RSType::rs_int4, {RSType::rs_int4, true, 4, "%int", "Rgba32i"}},
    {RSType::rs_float4, {RSType::rs_float4, true, 4, "%float", "Rgba32f"}}};
};

char ReflectionPass::ID = 0;

ModulePass *createReflectionPass(std::ostream &OS,
                                 bcinfo::MetadataExtractor &ME) {
  return new ReflectionPass(OS, ME);
}

bool ReflectionPass::emitHeader(const Module &M,
                                const KernelSignatures &Kernels) {
  DEBUG(dbgs() << "emitHeader\n");

  OS << "; SPIR-V\n"
        "; Version: 1.0\n"
        "; Generator: rs2spirv;\n"
        "; Bound: 1024\n"
        "; Schema: 0\n"
        "      OpCapability Shader\n"
        "      OpCapability StorageImageWriteWithoutFormat\n"
        "      OpCapability Addresses\n"
        " %glsl_ext_ins = OpExtInstImport \"GLSL.std.450\"\n"
        "      OpMemoryModel Physical32 GLSL450\n";
  for (const auto &Kernel : Kernels) {
    OS << "      OpEntryPoint GLCompute " << Kernel.getWrapperName() << " "
       << "\"" << Kernel.name << "\" %global_invocation_id\n";
  }
  for (const auto &Kernel : Kernels) {
    OS << "      OpExecutionMode " << Kernel.getWrapperName()
       << " LocalSize 1 1 1\n";
  }
  OS << "      OpSource GLSL 450\n"
        "      OpSourceExtension \"GL_ARB_separate_shader_objects\"\n"
        "      OpSourceExtension \"GL_ARB_shading_language_420pack\"\n"
        "      OpSourceExtension \"GL_GOOGLE_cpp_style_line_directive\"\n"
        "      OpSourceExtension \"GL_GOOGLE_include_directive\"\n";

  const size_t RSKernelNum = ME.getExportForEachSignatureCount();

  if (RSKernelNum == 0)
    return false;

  const char **RSKernelNames = ME.getExportForEachNameList();

  OS << " %RS_KERNELS = OpString \"";

  for (size_t i = 0; i < RSKernelNum; ++i)
    if (RSKernelNames[i] != StringRef("root"))
      OS << '%' << RSKernelNames[i] << " ";

  OS << "\"\n";

  return true;
}

bool ReflectionPass::emitDecorations(
    const Module &M, const SmallVectorImpl<RSAllocationInfo> &RSAllocs,
    const KernelSignatures &Kernels) {
  DEBUG(dbgs() << "emitDecorations\n");

  OS << "\n"
        "      OpDecorate %global_invocation_id BuiltIn GlobalInvocationId\n";
  for (const auto &K : Kernels) {
    OS << "      OpDecorate " << K.getTempName("input_image")
       << " DescriptorSet 0\n";
    OS << "      OpDecorate " << K.getTempName("input_image") << " Binding 0\n";
    OS << "      OpDecorate " << K.getTempName("input_image")
       << " NonWritable\n";
    OS << "      OpDecorate " << K.getTempName("output_image")
       << " DescriptorSet 0\n";
    OS << "      OpDecorate " << K.getTempName("output_image")
       << " Binding 1\n";
    OS << "      OpDecorate " << K.getTempName("output_image")
       << " NonReadable\n";
  }

  const auto GlobalsB = M.globals().begin();
  const auto GlobalsE = M.globals().end();
  const auto Found =
      std::find_if(GlobalsB, GlobalsE, [](const GlobalVariable &GV) {
        return GV.getName() == "__GPUBlock";
      });

  if (Found == GlobalsE)
    return true; // GPUBlock not found - not an error by itself.

  const GlobalVariable &G = *Found;

  DEBUG(dbgs() << "Found GPUBlock:\t");
  DEBUG(G.dump());

  bool IsCorrectTy = false;
  if (const auto *PtrTy = dyn_cast<PointerType>(G.getType())) {
    if (auto *StructTy = dyn_cast<StructType>(PtrTy->getElementType())) {
      IsCorrectTy = true;

      const auto &DLayout = M.getDataLayout();
      const auto *SLayout = DLayout.getStructLayout(StructTy);
      assert(SLayout);

      for (size_t i = 0, e = StructTy->getNumElements(); i != e; ++i)
        OS << "      OpMemberDecorate %rs_linker_struct___GPUBuffer " << i
           << " Offset " << SLayout->getElementOffset(i) << '\n';
    }
  }

  if (!IsCorrectTy) {
    errs() << "GPUBlock is not of expected type:\t";
    G.print(errs());
    G.getType()->print(errs());
    return false;
  }

  OS << "      OpDecorate %rs_linker_struct___GPUBuffer BufferBlock\n";
  OS << "      OpDecorate %rs_linker___GPUBlock DescriptorSet 0\n";
  OS << "      OpDecorate %rs_linker___GPUBlock Binding 2\n";

  size_t BindingNum = 3;

  for (const auto &A : RSAllocs) {
    OS << "      OpDecorate " << A.VarName << "_var DescriptorSet 0\n";
    OS << "      OpDecorate " << A.VarName << "_var Binding " << BindingNum
       << '\n';
    ++BindingNum;
  }

  return true;
}

void ReflectionPass::emitCommonTypes() {
  DEBUG(dbgs() << "emitCommonTypes\n");

  OS << "\n\n"
        "%void = OpTypeVoid\n"
        "%fun_void = OpTypeFunction %void\n"
        "%float = OpTypeFloat 32\n"
        "%v2float = OpTypeVector %float 2\n"
        "%v3float = OpTypeVector %float 3\n"
        "%v4float = OpTypeVector %float 4\n"
        "%int = OpTypeInt 32 1\n"
        "%v2int = OpTypeVector %int 2\n"
        "%v4int = OpTypeVector %int 4\n"
        "%uchar = OpTypeInt 8 0\n"
        "%v2uchar = OpTypeVector %uchar 2\n"
        "%v3uchar = OpTypeVector %uchar 3\n"
        "%v4uchar = OpTypeVector %uchar 4\n"
        "%uint = OpTypeInt 32 0\n"
        "%v2uint = OpTypeVector %uint 2\n"
        "%v3uint = OpTypeVector %uint 3\n"
        "%v4uint = OpTypeVector %uint 4\n"
        "%fun_f3_uc3 = OpTypeFunction %v3float %v3uchar\n"
        "%fun_f3_u3 = OpTypeFunction %v3float %v3uint\n"
        "%fun_f4_uc4 = OpTypeFunction %v4float %v4uchar\n"
        "%fun_uc3_f3 = OpTypeFunction %v3uchar %v3float\n"
        "%fun_u3_f3 = OpTypeFunction %v3uint %v3float\n"
        "%fun_uc4_f4 = OpTypeFunction %v4uchar %v4float\n"
        "%fun_uc4_u4 = OpTypeFunction %v4uchar %v4uint\n"
        "%fun_u4_uc4 = OpTypeFunction %v4uint %v4uchar\n"
        "%fun_f_f = OpTypeFunction %float %float\n"
        "%fun_f_ff = OpTypeFunction %float %float %float\n"
        "%fun_f_fff = OpTypeFunction %float %float %float %float\n"
        "%fun_f_f2f2 = OpTypeFunction %float %v2float %v2float\n"
        "%fun_f_f3f3 = OpTypeFunction %float %v3float %v3float\n"
        "%fun_f3_f3ff = OpTypeFunction %v3float %v3float %float %float\n"
        "%fun_i_iii = OpTypeFunction %int %int %int %int\n"
        "%fun_uc_uu = OpTypeFunction %uchar %uint %uint\n"
        "%fun_u_uu = OpTypeFunction %uint %uint %uint\n"
        "%fun_u_uuu = OpTypeFunction %uint %uint %uint %uint\n"
        "%fun_u3_u3uu = OpTypeFunction %v3uint %v3uint %uint %uint\n";
}

static Coords GetCoordsKind(const Function &F) {
  if (F.arg_size() <= 1)
    return Coords::None;

  DEBUG(F.getFunctionType()->dump());

  SmallVector<const Argument *, 4> Args;
  Args.reserve(F.arg_size());
  for (const auto &Arg : F.args())
    Args.push_back(&Arg);

  auto IsInt32 = [](const Argument *Arg) {
    assert(Arg);
    auto *Ty = Arg->getType();
    auto IntTy = dyn_cast<IntegerType>(Ty);
    if (!IntTy)
      return false;

    return IntTy->getBitWidth() == 32;
  };

  size_t LastInt32Num = 0;
  size_t XPos = -1; // npos - not found.
  auto RIt = Args.rbegin();
  const auto REnd = Args.rend();
  while (RIt != REnd && IsInt32(*RIt)) {
    if ((*RIt)->getName() == "x")
      XPos = Args.size() - 1 - LastInt32Num;

    ++LastInt32Num;
    ++RIt;
  }

  DEBUG(dbgs() << "Original number of last i32's: " << LastInt32Num << '\n');
  DEBUG(dbgs() << "X found at position: " << XPos << '\n');
  if (XPos == size_t(-1) || Args.size() - XPos > size_t(Coords::Last))
    return Coords::None;

  // Check remaining coordinate names.
  for (size_t i = 1, c = XPos + 1, e = Args.size(); c != e; ++i, ++c)
    if (Args[c]->getName() != CoordsNames[i])
      return Coords::None;

  DEBUG(dbgs() << "Coords: not none!\n");

  return Coords(Args.size() - XPos);
}

bool ReflectionPass::extractKernelSignatures(
    const Module &M, SmallVectorImpl<KernelSignature> &Out) {
  DEBUG(dbgs() << "extractKernelSignatures\n");

  for (const auto &F : M.functions()) {
    if (F.isDeclaration())
      continue;

    const auto CoordsKind = GetCoordsKind(F);
    const auto CoordsNum = unsigned(CoordsKind);
    if (F.arg_size() != CoordsNum + 1) {
      // TODO: Handle different arrities (and lack of return value).
      errs() << "Unsupported kernel signature.\n";
      return false;
    }

    const auto *FT = F.getFunctionType();
    const auto *RT = FT->getReturnType();
    const auto *ArgT = FT->params()[0];
    Out.push_back(
        {TypeToString(RT), F.getName(), TypeToString(ArgT), GetCoordsKind(F)});
    DEBUG(Out.back().dump());
  }

  return true;
}

bool ReflectionPass::emitKernelTypes(const KernelSignature &Kernel) {
  DEBUG(dbgs() << "emitKernelTypes\n");

  const auto *RTMapping = getMappingOrPrintError(Kernel.returnType);
  const auto *ArgTMapping = getMappingOrPrintError(Kernel.argumentType);

  if (!RTMapping || !ArgTMapping)
    return false;

  OS << '\n'
     << Kernel.getTempName("kernel_function_ty") << " = OpTypeFunction "
     << RTMapping->SPIRVTy << ' ' << ArgTMapping->SPIRVTy;

  const auto CoordsNum = unsigned(Kernel.coordsKind);
  for (size_t i = 0; i != CoordsNum; ++i)
    OS << " %uint";

  OS << '\n';

  OS << Kernel.getTempName("ptr_function_ty") << " = OpTypePointer Function "
     << RTMapping->SPIRVTy << "\n";
  OS << Kernel.getTempName("ptr_function_access_ty")
     << " = OpTypePointer Function " << RTMapping->SPIRVImageReadType << "\n\n";

  return true;
}

bool ReflectionPass::emitInputImage(const KernelSignature &Kernel) {
  DEBUG(dbgs() << "emitInputImage\n");

  const auto *ArgTMapping = getMappingOrPrintError(Kernel.argumentType);
  if (!ArgTMapping)
    return false;

  OS << Kernel.getTempName("input_image_ty") << " = OpTypeImage "
     << ArgTMapping->SPIRVScalarTy << " 2D 0 0 0 2 "
     << ArgTMapping->SPIRVImageFormat << '\n';

  OS << Kernel.getTempName("input_image_ptr_ty")
     << " = OpTypePointer UniformConstant "
     << Kernel.getTempName("input_image_ty") << "\n";

  OS << Kernel.getTempName("input_image") << " = OpVariable "
     << Kernel.getTempName("input_image_ptr_ty") << " UniformConstant\n";

  return true;
}

void ReflectionPass::emitGLGlobalInput() {
  DEBUG(dbgs() << "emitGLGlobalInput\n");

  OS << '\n'
     << "%global_input_ptr_ty = OpTypePointer Input %v3uint\n"
     << "%global_invocation_id = OpVariable %global_input_ptr_ty Input\n";
}

bool ReflectionPass::emitOutputImage(const KernelSignature &Kernel) {
  DEBUG(dbgs() << "emitOutputImage\n");

  const auto *RTMapping = getMappingOrPrintError(Kernel.returnType);
  if (!RTMapping)
    return false;

  OS << '\n';
  OS << Kernel.getTempName("output_image_ty") << " = OpTypeImage "
     << RTMapping->SPIRVScalarTy << " 2D 0 0 0 2 "
     << RTMapping->SPIRVImageFormat << '\n'
     << Kernel.getTempName("output_image_ptr_ty")
     << " = OpTypePointer UniformConstant "
     << Kernel.getTempName("output_image_ty") << "\n";

  OS << Kernel.getTempName("output_image") << " = OpVariable "
     << Kernel.getTempName("output_image_ptr_ty") << " Image\n";

  return true;
}

bool ReflectionPass::emitRSAllocImages(
    const SmallVectorImpl<RSAllocationInfo> &RSAllocs) {
  DEBUG(dbgs() << "emitRSAllocImages\n");

  for (const auto &A : RSAllocs) {
    if (!A.RSElementType) {
      errs() << "Type of variable " << A.VarName << " not infered.\n";
      return false;
    }

    const auto *AMapping = getMappingOrPrintError(*A.RSElementType);
    if (!AMapping)
      return false;

    OS << '\n'
       << A.VarName << "_image_ty"
       << " = OpTypeImage " << AMapping->SPIRVScalarTy << " 2D 0 0 0 2 "
       << AMapping->SPIRVImageFormat << '\n'
       << A.VarName << "_image_ptr_ty"
       << " = OpTypePointer UniformConstant " << A.VarName << "_image_ty\n";

    OS << A.VarName << "_var = OpVariable " << A.VarName
       << "_image_ptr_ty Image\n";
  }

  return true;
}

bool ReflectionPass::emitConstants(const KernelSignature &Kernel) {
  DEBUG(dbgs() << "emitConstants\n");

  OS << "\n"
        "%uint_zero = OpConstant %uint 0\n"
        "%float_zero = OpConstant %float 0\n";

  return true;
}

static std::string GenerateConversionFun(const char *Name, const char *FType,
                                         const char *From, const char *To,
                                         const char *ConversionOp) {
  std::ostringstream OS;

  OS << "\n"
     << "%rs_linker_" << Name << " = OpFunction " << To << " Pure " << FType
     << "\n"
     << "%param" << Name << " = OpFunctionParameter " << From << "\n"
     << "%label" << Name << " = OpLabel\n"
     << "%res" << Name << " = " << ConversionOp << " " << To << " %param"
     << Name << "\n"
     << "      OpReturnValue %res" << Name << "\n"
     << "      OpFunctionEnd\n";

  return OS.str();
}

static std::string GenerateEISFun(const char *Name, const char *FType,
                                  const char *RType,
                                  const SmallVector<const char *, 4> &ArgTypes,
                                  const char *InstName) {
  std::ostringstream OS;

  OS << '\n'
     << "%rs_linker_" << Name << " = OpFunction " << RType << " Pure " << FType
     << '\n';

  for (size_t i = 0, e = ArgTypes.size(); i < e; ++i)
    OS << "%param" << Name << i << " = OpFunctionParameter " << ArgTypes[i]
       << "\n";

  OS << "%label" << Name << " = OpLabel\n"
     << "%res" << Name << " = "
     << "OpExtInst " << RType << " %glsl_ext_ins " << InstName;

  for (size_t i = 0, e = ArgTypes.size(); i < e; ++i)
    OS << " %param" << Name << i;

  OS << '\n'
     << "      OpReturnValue %res" << Name << "\n"
     << "      OpFunctionEnd\n";

  return OS.str();
}

// This SPIRV function generator relies heavily on future inlining.
// Currently, the inliner doesn't perform any type checking - it blindly
// maps function parameters to supplied parameters at call site.
// It's non-trivial to generate correct SPIRV function signature based only
// on the LLVM one, and the current design doesn't allow lazy type generation.
//
// TODO: Consider less horrible generator design that doesn't rely on lack of
// type checking in the inliner.
static std::string GenerateRSGEA(const char *Name, const char *RType,
                                 StringRef LoadName, Coords CoordsKind) {
  assert(CoordsKind != Coords::None);
  std::ostringstream OS;

  OS << "\n"
     << "%rs_linker_" << Name << " = OpFunction " << RType
     << " None %rs_inliner_placeholder_ty\n";

  // Since the inliner doesn't perform type checking, function and parameter
  // types can be anything. %rs_inliner_placeholder_ty is just a placeholder
  // name that will disappear after inlining.

  OS << "%rs_drop_param_" << Name << " = OpFunctionParameter "
     << "%rs_inliner_placeholder_ty\n";

  for (size_t i = 0, e = size_t(CoordsKind); i != e; ++i)
    OS << "%param" << Name << '_' << CoordsNames[i].str()
       << " = OpFunctionParameter %uint\n";

  OS << "%label" << Name << " = OpLabel\n";
  OS << "%arg" << Name << " = OpCompositeConstruct %v" << size_t(CoordsKind)
     << "uint ";

  for (size_t i = 0, e = size_t(CoordsKind); i != e; ++i)
    OS << "%param" << Name << '_' << CoordsNames[i].str() << ' ';

  OS << '\n';

  OS << "%read" << Name << " = OpImageRead " << RType << ' ' << LoadName.str()
     << " %arg" << Name << '\n';
  OS << "      OpReturnValue %read" << Name << '\n';
  OS << "      OpFunctionEnd\n";

  return OS.str();
}

// The same remarks as to GenerateRSGEA apply to SEA function generator.
static std::string GenerateRSSEA(const char *Name, StringRef LoadName,
                                 Coords CoordsKind) {
  assert(CoordsKind != Coords::None);
  std::ostringstream OS;

  // %rs_inliner_placeholder_ty will disappear after inlining.
  OS << "\n"
     << "%rs_linker_" << Name << " = OpFunction %void None "
     << "%rs_inliner_placeholder_ty\n";

  OS << "%rs_placeholder_param_" << Name << " = OpFunctionParameter "
     << "%rs_inliner_placeholder_ty\n";
  OS << "%param" << Name << "_new_val = OpFunctionParameter "
     << "%rs_inliner_placeholder_ty\n";

  for (size_t i = 0, e = size_t(CoordsKind); i != e; ++i)
    OS << "%param" << Name << '_' << CoordsNames[i].str()
       << " = OpFunctionParameter %uint\n";

  OS << "%label" << Name << " = OpLabel\n";
  OS << "%arg" << Name << " = OpCompositeConstruct %v" << size_t(CoordsKind)
     << "uint ";

  for (size_t i = 0, e = size_t(CoordsKind); i != e; ++i)
    OS << "%param" << Name << '_' << CoordsNames[i].str() << ' ';

  OS << '\n';

  OS << "OpImageWrite " << LoadName.str() << " %arg" << Name << " %param"
     << Name << "_new_val\n";
  OS << "      OpReturn\n";
  OS << "      OpFunctionEnd\n";

  return OS.str();
}

void ReflectionPass::emitRTFunctions() {
  DEBUG(dbgs() << "emitRTFunctions\n");

  // TODO: Emit other runtime functions.
  // TODO: Generate libary file instead of generating functions below
  // every compilation.

  // Use uints as Khronos' SPIRV converter turns LLVM's i32s into uints.

  OS << GenerateConversionFun("_Z14convert_float4Dv4_h", "%fun_f4_uc4",
                              "%v4uchar", "%v4float", "OpConvertUToF");

  OS << GenerateConversionFun("_Z14convert_uchar4Dv4_f", "%fun_uc4_f4",
                              "%v4float", "%v4uchar", "OpConvertFToU");

  OS << GenerateConversionFun("_Z14convert_float3Dv3_h", "%fun_f3_uc3",
                              "%v3uchar", "%v3float", "OpConvertUToF");

  OS << GenerateConversionFun("_Z14convert_uchar3Dv3_f", "%fun_uc3_f3",
                              "%v3float", "%v3uchar", "OpConvertFToU");

  OS << GenerateConversionFun("_Z12convert_int3Dv3_f", "%fun_u3_f3", "%v3float",
                              "%v3uint", "OpConvertFToU");

  OS << GenerateConversionFun("_Z14convert_uchar3Dv3_i", "%fun_uc3_u3",
                              "%v3uint", "%v3uchar", "OpUConvert");

  OS << GenerateConversionFun("_Z14convert_uchar4Dv4_j", "%fun_uc4_u4",
                              "%v4uint", "%v4uchar", "OpUConvert");

  OS << GenerateConversionFun("_Z13convert_uint4Dv4_h", "%fun_u4_uc4",
                              "%v4uchar", "%v4uint", "OpUConvert");

  OS << GenerateEISFun("_Z3sinf", "%fun_f_f", "%float", {"%float"}, "Sin");
  OS << GenerateEISFun("_Z4sqrtf", "%fun_f_f", "%float", {"%float"}, "Sqrt");
  OS << GenerateEISFun("_Z10native_expf", "%fun_f_f", "%float", {"%float"},
                       "Exp");
  OS << GenerateEISFun("_Z3maxii", "%fun_u_uu", "%uint", {"%uint", "%uint"},
                       "SMax");
  OS << GenerateEISFun("_Z3minii", "%fun_u_uu", "%uint", {"%uint", "%uint"},
                       "SMin");
  OS << GenerateEISFun("_Z3maxff", "%fun_f_ff", "%float", {"%float", "%float"},
                       "FMax");
  OS << GenerateEISFun("_Z3minff", "%fun_f_ff", "%float", {"%float", "%float"},
                       "FMin");
  OS << GenerateEISFun("_Z5clampfff", "%fun_f_fff", "%float",
                       {"%float", "%float", "%float"}, "FClamp");
  OS << GenerateEISFun("_Z5clampiii", "%fun_u_uuu", "%uint",
                       {"%uint", "%uint", "%uint"}, "SClamp");

  OS << R"(
%rs_linker__Z3dotDv2_fS_ = OpFunction %float Pure %fun_f_f2f2
%param_Z3dotDv2_fS_0 = OpFunctionParameter %v2float
%param_Z3dotDv2_fS_1 = OpFunctionParameter %v2float
%label_Z3dotDv2_fS = OpLabel
%res_Z3dotDv2_fS = OpDot %float %param_Z3dotDv2_fS_0 %param_Z3dotDv2_fS_1
      OpReturnValue %res_Z3dotDv2_fS
      OpFunctionEnd
)";

  OS << R"(
%rs_linker__Z3dotDv3_fS_ = OpFunction %float Pure %fun_f_f3f3
%param_Z3dotDv3_fS_0 = OpFunctionParameter %v3float
%param_Z3dotDv3_fS_1 = OpFunctionParameter %v3float
%label_Z3dotDv3_fS = OpLabel
%res_Z3dotDv3_fS = OpDot %float %param_Z3dotDv3_fS_0 %param_Z3dotDv3_fS_1
      OpReturnValue %res_Z3dotDv3_fS
      OpFunctionEnd
)";

  OS << R"(
%rs_linker_rsUnpackColor8888 = OpFunction %v4float Pure %fun_f4_uc4
%paramrsUnpackColor88880 = OpFunctionParameter %v4uchar
%labelrsUnpackColor8888 = OpLabel
%castedUnpackColor8888 = OpBitcast %uint %paramrsUnpackColor88880
%resrsUnpackColor8888 = OpExtInst %v4float %glsl_ext_ins UnpackUnorm4x8 %castedUnpackColor8888
      OpReturnValue %resrsUnpackColor8888
      OpFunctionEnd
)";

  OS << R"(
%rs_linker__Z17rsPackColorTo8888Dv4_f = OpFunction %v4uchar Pure %fun_uc4_f4
%param_Z17rsPackColorTo8888Dv4_f0 = OpFunctionParameter %v4float
%label_Z17rsPackColorTo8888Dv4_f = OpLabel
%res_Z17rsPackColorTo8888Dv4_f = OpExtInst %uint %glsl_ext_ins PackUnorm4x8 %param_Z17rsPackColorTo8888Dv4_f0
%casted_Z17rsPackColorTo8888Dv4_f = OpBitcast %v4uchar %res_Z17rsPackColorTo8888Dv4_f
      OpReturnValue %casted_Z17rsPackColorTo8888Dv4_f
      OpFunctionEnd
)";

  OS << R"(
%rs_linker__Z5clampDv3_fff = OpFunction %v3float Pure %fun_f3_f3ff
%param_Z5clampDv3_fff0 = OpFunctionParameter %v3float
%param_Z5clampDv3_fff1 = OpFunctionParameter %float
%param_Z5clampDv3_fff2 = OpFunctionParameter %float
%label_Z5clampDv3_fff = OpLabel
%arg1_Z5clampDv3_fff = OpCompositeConstruct %v3float %param_Z5clampDv3_fff1 %param_Z5clampDv3_fff1 %param_Z5clampDv3_fff1
%arg2_Z5clampDv3_fff = OpCompositeConstruct %v3float %param_Z5clampDv3_fff2 %param_Z5clampDv3_fff2 %param_Z5clampDv3_fff2
%res_Z5clampDv3_fff = OpExtInst %v3float %glsl_ext_ins FClamp %param_Z5clampDv3_fff0 %arg1_Z5clampDv3_fff %arg2_Z5clampDv3_fff
      OpReturnValue %res_Z5clampDv3_fff
      OpFunctionEnd
)";

  OS << R"(
%rs_linker__Z5clampDv3_iii = OpFunction %v3uint Pure %fun_u3_u3uu
%param_Z5clampDv3_iii0 = OpFunctionParameter %v3uint
%param_Z5clampDv3_iii1 = OpFunctionParameter %uint
%param_Z5clampDv3_iii2 = OpFunctionParameter %uint
%label_Z5clampDv3_iii = OpLabel
%arg1_Z5clampDv3_iii = OpCompositeConstruct %v3uint %param_Z5clampDv3_iii1 %param_Z5clampDv3_iii1 %param_Z5clampDv3_iii1
%arg2_Z5clampDv3_iii = OpCompositeConstruct %v3uint %param_Z5clampDv3_iii2 %param_Z5clampDv3_iii2 %param_Z5clampDv3_iii2
%res_Z5clampDv3_iii = OpExtInst %v3uint %glsl_ext_ins UClamp %param_Z5clampDv3_iii0 %arg1_Z5clampDv3_iii %arg2_Z5clampDv3_iii
      OpReturnValue %res_Z5clampDv3_iii
      OpFunctionEnd
)";
}

bool ReflectionPass::emitRSAllocFunctions(
    Module &M, const SmallVectorImpl<RSAllocationInfo> &RSAllocs,
    const SmallVectorImpl<RSAllocationCallInfo> &RSAllocAccesses) {
  DEBUG(dbgs() << "emitRSAllocFunctions\n");

  for (const auto &Access : RSAllocAccesses) {
    solidifyRSAllocAccess(M, Access);

    auto *Fun = Access.FCall->getCalledFunction();
    if (!Fun)
      return false;

    const auto FName = Fun->getName();
    auto *ETMapping = getMappingOrPrintError(Access.RSElementTy);
    if (!ETMapping)
      return false;

    const auto ElementTy = ETMapping->SPIRVTy;
    const std::string LoadName = Access.RSAlloc.VarName + "_load";

    if (Access.Kind == RSAllocAccessKind::GEA)
      OS << GenerateRSGEA(FName.str().c_str(), ElementTy.c_str(),
                          LoadName.c_str(), Coords::XY);
    else
      OS << GenerateRSSEA(FName.str().c_str(), LoadName.c_str(), Coords::XY);
  }

  return true;
}

bool ReflectionPass::emitMain(
    const KernelSignature &Kernel,
    const SmallVectorImpl<RSAllocationInfo> &RSAllocs) {
  DEBUG(dbgs() << "emitMain\n");

  const auto *RTMapping = getMappingOrPrintError(Kernel.returnType);
  const auto *ArgTMapping = getMappingOrPrintError(Kernel.argumentType);

  if (!RTMapping || !ArgTMapping)
    return false;

  // Produce boilerplate code for the kernel with unique SPIR-V textual IDs
  OS << '\n';
  OS << Kernel.getWrapperName() << " = OpFunction %void None %fun_void\n";

  OS << Kernel.getTempName("label") << " = OpLabel\n";

  OS << Kernel.getTempName("input_pixel") << " = OpVariable "
     << Kernel.getTempName("ptr_function_access_ty") << " Function\n";

  OS << Kernel.getTempName("res") << " = OpVariable "
     << Kernel.getTempName("ptr_function_ty") << " Function\n";

  OS << Kernel.getTempName("image_load") << " = OpLoad "
     << Kernel.getTempName("input_image_ty") << " "
     << Kernel.getTempName("input_image") << "\n";

  OS << Kernel.getTempName("coords_load")
     << " = OpLoad %v3uint %global_invocation_id\n";

  OS << Kernel.getTempName("coords_x") << " = OpCompositeExtract %uint "
     << Kernel.getTempName("coords_load") << " 0\n";

  OS << Kernel.getTempName("coords_y") << " = OpCompositeExtract %uint "
     << Kernel.getTempName("coords_load") << " 1\n";

  OS << Kernel.getTempName("coords_z") << " = OpCompositeExtract %uint "
     << Kernel.getTempName("coords_load") << " 2\n";

  OS << Kernel.getTempName("shuffled") << " = OpVectorShuffle %v2uint "
     << Kernel.getTempName("coords_load") << " "
     << Kernel.getTempName("coords_load") << " 0 1\n";

  OS << Kernel.getTempName("bitcasted") << " = OpBitcast %v2int "
     << Kernel.getTempName("shuffled") << "\n";

  OS << Kernel.getTempName("image_read") << " = OpImageRead "
     << ArgTMapping->SPIRVImageReadType << " "
     << Kernel.getTempName("image_load") << " "
     << Kernel.getTempName("bitcasted") << "\n";

  OS << "  OpStore " << Kernel.getTempName("input_pixel") << " "
     << Kernel.getTempName("image_read") << "\n";

  // TODO: Handle vector types of width different than 4.
  if (RTMapping->isVectorTy) {
    OS << " " << Kernel.getTempName("input_load") << " = OpLoad "
       << ArgTMapping->SPIRVTy << " " << Kernel.getTempName("input_pixel")
       << "\n";
  } else {
    OS << Kernel.getTempName("input_access_chain") << " = OpAccessChain "
       << Kernel.getTempName("ptr_function_ty") << " "
       << Kernel.getTempName("input_pixel") << " %uint_zero\n";

    OS << Kernel.getTempName("input_load") << " = OpLoad "
       << ArgTMapping->SPIRVTy << " "
       << Kernel.getTempName("input_access_chain") << "\n";
  }

  for (const auto &A : RSAllocs)
    OS << A.VarName << "_load = OpLoad " << A.VarName << "_image_ty "
       << A.VarName << "_var\n";

  OS << Kernel.getTempName("kernel_call") << " = OpFunctionCall "
     << ArgTMapping->SPIRVTy << " %RS_SPIRV_DUMMY_ "
     << Kernel.getTempName("input_load");

  const auto CoordsNum = size_t(Kernel.coordsKind);
  for (size_t i = 0; i != CoordsNum; ++i)
    OS << " " << Kernel.getTempName("coords_") << CoordsNames[i].str();

  OS << '\n';

  OS << " OpStore " << Kernel.getTempName("res") << " "
     << Kernel.getTempName("kernel_call") << "\n";

  OS << Kernel.getTempName("output_load") << " = OpLoad "
     << Kernel.getTempName("input_image_ty") << " "
     << Kernel.getTempName("output_image") << "\n";

  OS << " " << Kernel.getTempName("res_load") << " = OpLoad "
     << RTMapping->SPIRVTy << " " << Kernel.getTempName("res") << "\n";

  if (!RTMapping->isVectorTy) {
    OS << Kernel.getTempName("composite_constructed")
       << " = OpCompositeConstruct " << RTMapping->SPIRVImageReadType;
    for (size_t i = 0; i < RTMapping->vectorWidth; ++i)
      OS << " " << Kernel.getTempName("res_load");

    OS << "\n";
    OS << " OpImageWrite " << Kernel.getTempName("output_load") << " "
       << Kernel.getTempName("bitcasted") << " "
       << Kernel.getTempName("composite_constructed") << "\n";
  } else {
    OS << " OpImageWrite " << Kernel.getTempName("output_load") << " "
       << Kernel.getTempName("bitcasted") << " "
       << Kernel.getTempName("res_load") << "\n";
  }

  OS << "               OpReturn\n"
        "               OpFunctionEnd\n";

  OS << "%RS_SPIRV_DUMMY_ = OpFunction " << RTMapping->SPIRVTy << " None "
     << Kernel.getTempName("kernel_function_ty") << "\n";

  OS << "          %p = OpFunctionParameter " << ArgTMapping->SPIRVTy << '\n';

  for (size_t i = 0; i != CoordsNum; ++i)
    OS << "          %coords_param_" << CoordsNames[i].str()
       << " = OpFunctionParameter %uint\n";

  OS << "         %11 = OpLabel\n"
        "               OpReturnValue %p\n"
        "               OpFunctionEnd\n";

  return true;
}

} // namespace rs2spirv
