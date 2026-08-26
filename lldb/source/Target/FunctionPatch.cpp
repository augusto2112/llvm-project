//===-- FunctionPatch.cpp -------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/FunctionPatch.h"
#include "lldb/Target/Target.h"
#include <chrono>

using namespace lldb_private;

namespace {
/// A build writes the binary after the sources it read, and a filesystem with
/// coarse timestamps can leave the two within a second of each other either
/// way. A refusal that fires on that noise would refuse every ordinary build.
constexpr std::chrono::seconds kMinSourceSkew{2};
} // namespace

namespace lldb_private {

struct FunctionPatchManager::PatchedFunction {
  // Will be filled in later when the install path is implemented
};

} // namespace lldb_private

llvm::StringRef lldb_private::ToString(PatchFailure Reason) {
  switch (Reason) {
  case PatchFailure::NotArm64:
    return "in-process evaluation is implemented for arm64 only";
  case PatchFailure::NoProcess:
    return "there is no running process to patch";
  case PatchFailure::NoSourceFile:
    return "the function's source file could not be read";
  case PatchFailure::SourceNewerThanBinary:
    return "the source on disk was written after the binary, so recompiling "
           "it would substitute a different function";
  case PatchFailure::BodyNotFound:
    return "the function's body could not be located in its source";
  case PatchFailure::StaticLocal:
    return "the body declares a static local, which a copy cannot share";
  case PatchFailure::EntryTooSmall:
    return "the function is too small to hold a redirect";
  case PatchFailure::ThreadInPatchRange:
    return "a thread is stopped inside the bytes the redirect would overwrite";
  case PatchFailure::BreakpointInPatchRange:
    return "a breakpoint sits inside the bytes the redirect would overwrite";
  case PatchFailure::CompileFailed:
    return "the recompiled function did not compile";
  case PatchFailure::CaptureNotScalar:
    return "the capture is not a scalar of eight bytes or fewer";
  case PatchFailure::Unsupported:
    return "the observation asks for something in-process evaluation does not "
           "implement";
  }
  return "unknown";
}

bool lldb_private::SourceSkewExceedsNoise(llvm::sys::TimePoint<> Source,
                                         llvm::sys::TimePoint<> Binary) {
  if (Source == llvm::sys::TimePoint<>() || Binary == llvm::sys::TimePoint<>())
    return false;
  return Source - Binary >= kMinSourceSkew;
}

FunctionPatchManager::FunctionPatchManager(Target &Tgt) : m_target(Tgt) {}

FunctionPatchManager::~FunctionPatchManager() = default;

llvm::Expected<uint32_t> FunctionPatchManager::Install(const PatchRequest &Request) {
  return llvm::make_error<llvm::StringError>(
      "not yet implemented",
      llvm::inconvertibleErrorCode());
}

llvm::Error FunctionPatchManager::Remove(uint32_t SiteID) {
  return llvm::make_error<llvm::StringError>(
      "not yet implemented",
      llvm::inconvertibleErrorCode());
}

void FunctionPatchManager::SetGate(uint32_t SiteID, bool Open) {
  // not yet implemented
}

bool FunctionPatchManager::IsPatched(lldb::addr_t Entry) const {
  return m_functions.find(Entry) != m_functions.end();
}

llvm::Error FunctionPatchManager::EnsureRingBlock() {
  return llvm::make_error<llvm::StringError>(
      "not yet implemented",
      llvm::inconvertibleErrorCode());
}

llvm::Expected<lldb::addr_t> FunctionPatchManager::AllocateSiteSlot() {
  return llvm::make_error<llvm::StringError>(
      "not yet implemented",
      llvm::inconvertibleErrorCode());
}

llvm::Error FunctionPatchManager::Recompile(PatchedFunction &Fn) {
  return llvm::make_error<llvm::StringError>(
      "not yet implemented",
      llvm::inconvertibleErrorCode());
}
