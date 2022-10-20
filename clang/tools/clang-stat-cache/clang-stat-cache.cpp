//===- clang-stat-cache.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/attr.h>
#include <sys/errno.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vnode.h>
#include <unistd.h>

#include <algorithm>
#include <list>
#include <set>

#ifdef __APPLE__
#include <CoreServices/CoreServices.h>

#include <sys/mount.h>
#include <sys/param.h>
#endif // __APPLE__

// The clang-stat-cache utility creates an on-disk cache for the stat data
// of a file-system tree which is expected to be immutable during a build.

using namespace llvm;
using llvm::vfs::StatCacheFileSystem;

cl::OptionCategory StatCacheCategory("clang-stat-cache options");

cl::opt<std::string> OutputFilename("o", cl::Required,
                                    cl::desc("Specify output filename"),
                                    cl::value_desc("filename"),
                                    cl::cat(StatCacheCategory));

cl::opt<std::string> TargetDirectory(cl::Positional, cl::Required,
                                     cl::value_desc("dirname"),
                                     cl::cat(StatCacheCategory));

cl::opt<bool> Verbose("v", cl::desc("More verbose output"));
cl::opt<bool> Force("f", cl::desc("Force cache generation"));

namespace {

#if __APPLE__
// Used by checkContentsValidity. See below.
struct CallbackInfo {
  FSEventStreamEventId LastEvent;
  bool SeenChanges = false;
};

// Used by checkContentsValidity. See below.
void FSEventsCallback(ConstFSEventStreamRef streamRef, void *CtxInfo,
                      size_t numEvents, void *eventPaths,
                      const FSEventStreamEventFlags *eventFlags,
                      const FSEventStreamEventId *eventIds) {
  CallbackInfo *Info = static_cast<CallbackInfo *>(CtxInfo);
  for (size_t i = 0; i < numEvents; ++i) {
    Info->LastEvent = eventIds[i];
    // The kFSEventStreamEventFlagHistoryDone is set on the last 'historical'
    // event passed to the callback. This means it is passed after the callback
    // all the relevant activity between the StartEvent of the stream and the
    // point the stream was created.
    // If the callback didn't see any other event, it means there haven't been
    // any alterations to the target directory hierarchy and the cache contents
    // is still up-to-date.
    if (eventFlags[i] & kFSEventStreamEventFlagHistoryDone) {
      // Let's stop the main queue and go back to our non-queue code.
      CFRunLoopStop(CFRunLoopGetCurrent());
      break;
    }

    // If we see any event outisde of the kFSEventStreamEventFlagHistoryDone
    // one, there have been changes to the target directory.
    Info->SeenChanges = true;
  }
}

// FSEvents-based check for cache contents validity. We store the latest
// FSEventStreamEventId in the cache as a ValidityToken and check if any
// file system events affected the base directory since the cache was
// generated.
bool checkContentsValidity(uint64_t &ValidityToken) {
  CFStringRef TargetDir = CFStringCreateWithCStringNoCopy(
      kCFAllocatorDefault, TargetDirectory.c_str(), kCFStringEncodingASCII,
      kCFAllocatorNull);
  CFArrayRef PathsToWatch =
      CFArrayCreate(nullptr, (const void **)&TargetDir, 1, nullptr);
  CallbackInfo Info;
  FSEventStreamContext Ctx = {0, &Info, nullptr, nullptr, nullptr};
  FSEventStreamRef Stream;
  CFAbsoluteTime Latency = 0; // Latency in seconds. Do not wait.

  FSEventStreamEventId StartEvent = ValidityToken;

  // Create the stream
  Stream = FSEventStreamCreate(
      NULL,
      &FSEventsCallback,                    // Callback function
      &Ctx, PathsToWatch, StartEvent,       // Or a previous event ID
      Latency, kFSEventStreamCreateFlagNone // Flags explained in reference
  );

  // Associate the stream with the main queue.
  FSEventStreamSetDispatchQueue(Stream, dispatch_get_main_queue());
  // Start the stream (needs the queue to run to do anything).
  if (!FSEventStreamStart(Stream)) {
    errs() << "Failed to create FS event stream. "
           << "Considering the cache up-to-date.\n";
    return true;
  }

  // Start the main queue. It will be exited by our callback when it got
  // confirmed it processed all events.
  CFRunLoopRun();

  // Update the Validity token with the new last event we processed.
  ValidityToken = Info.LastEvent;
  return !Info.SeenChanges;
}

#else // __APPLE__

// There is no cross-platform way to implement a validity check. If this
// platofrm doesn't support it, just consider the cache constents always
// valid. When that's the case, the tool running cache generation needs
// to have the knowledge to do it only when needed.
bool checkContentsValidity(uint64_t &ValidityToken) { return true; }

#endif // __APPLE__

// Populate Generator with the stat cache data for the filesystem tree
// rooted at BasePath.
std::error_code
populateHashTable(StringRef BasePath,
                  StatCacheFileSystem::StatCacheWriter &Generator) {
  using namespace llvm;
  using namespace sys::fs;

  std::error_code ErrorCode;

  // Just loop over the target directory using a recursive iterator.
  // This invocation follows symlinks, so we are going to potentially
  // store the status of the same file multiple times with different
  // names.
  for (recursive_directory_iterator I(BasePath, ErrorCode), E;
       I != E && !ErrorCode; I.increment(ErrorCode)) {
    StringRef Path = I->path();
    sys::fs::file_status s;
    // This can fail (broken symlink) and leave the file_status with
    // its default values. The reader knows this.
    status(Path, s);

    Generator.addEntry(Path, s);
  }

  return ErrorCode;
}

bool checkCacheValid(int FD, raw_fd_ostream &Out, uint64_t &ValidityToken) {
  sys::fs::file_status Status;
  auto EC = sys::fs::status(FD, Status);
  if (EC) {
    llvm::errs() << "fstat failed\n";
    return false;
  }

  auto Size = Status.getSize();
  if (Size == 0) {
    // New file.
#ifdef __APPLE__
    // Get the current (global) FSEvent id and use this as ValidityToken.
    ValidityToken = FSEventsGetCurrentEventId();
#endif
    return false;
  }

  auto ErrorOrBuffer =
      MemoryBuffer::getOpenFile(FD, OutputFilename, Status.getSize());

  // Refuse to write to this cache file if it exists but its contents do
  // not look like a valid cache file.
  StringRef BaseDir;
  bool IsCaseSensitive;
  if (auto E = StatCacheFileSystem::validateCacheFile(
          (*ErrorOrBuffer)->getMemBufferRef(), BaseDir, IsCaseSensitive,
          ValidityToken)) {
    llvm::errs() << "The output cache file exists and is not a valid stat "
                    "cache.";
    if (!Force) {
      llvm::errs() << " Aborting.\n";
      exit(1);
    }

    consumeError(std::move(E));
    llvm::errs() << " Forced update.\n";
    return false;
  }

  if (BaseDir != TargetDirectory &&
      (IsCaseSensitive || !BaseDir.equals_insensitive(TargetDirectory))) {
    llvm::errs() << "Existing cache has different directory. Regenerating...\n";
    return false;
  }

  // Basic structure checks have passed. Lets see if we can prove that the cache
  // contents are still valid.
  bool IsValid = checkContentsValidity(ValidityToken);
  if (IsValid) {
    // The cache is valid, but we might have gotten an updated ValidityToken.
    // Update the cache with it as clang-stat-cache is just going to exit after
    // returning from this function.
    StatCacheFileSystem::updateValidityToken(Out, ValidityToken);
  }
  return IsValid && !Force;
}

} // namespace

int main(int argc, char *argv[]) {
  cl::ParseCommandLineOptions(argc, argv);

  // Remove extraneous separators from the end of the basename.
  while (!TargetDirectory.empty() &&
         sys::path::is_separator(TargetDirectory.back()))
    TargetDirectory.pop_back();
  StringRef Dirname(TargetDirectory);

  std::error_code EC;
  int FD;
  EC = sys::fs::openFileForReadWrite(
      OutputFilename, FD, llvm::sys::fs::CD_OpenAlways, llvm::sys::fs::OF_None);
  if (EC) {
    llvm::errs() << "Failed to open cache file: "
                 << toString(llvm::createFileError(OutputFilename, EC)) << "\n";
    return 1;
  }

  raw_fd_ostream Out(FD, /* ShouldClose=*/true);

  uint64_t ValidityToken = 0;
  // Check if the cache is valid and up-to-date.
  if (checkCacheValid(FD, Out, ValidityToken)) {
    if (Verbose)
      outs() << "Cache up-to-date, exiting\n";
    return 0;
  }

  if (Verbose)
    outs() << "Building a stat cache for '" << TargetDirectory << "' into '"
           << OutputFilename << "'\n";

  // Do not generate a cache for NFS. Iterating huge directory hierarchies
  // over NFS will be very slow. Better to let the compiler search only the
  // pieces that it needs than use a cache that takes ages to populate.
  bool IsLocal;
  EC = sys::fs::is_local(Dirname, IsLocal);
  if (EC) {
    errs() << "Failed to stat the target directory: "
           << llvm::toString(llvm::errorCodeToError(EC)) << "\n";
    return 1;
  }

  if (!IsLocal) {
    errs() << "Target directory is not a local filesystem. "
           << "Not populating the cache.\n";
    return 0;
  }

  // Check if the filesystem hosting the target directory is case sensitive.
  bool IsCaseSensitive = true;
#ifdef _PC_CASE_SENSITIVE
  IsCaseSensitive =
      ::pathconf(TargetDirectory.c_str(), _PC_CASE_SENSITIVE) == 1;
#endif
  StatCacheFileSystem::StatCacheWriter Generator(Dirname, IsCaseSensitive,
                                                 ValidityToken);

  // Populate the cache.
  auto startTime = llvm::TimeRecord::getCurrentTime();
  populateHashTable(Dirname, Generator);
  auto endTime = llvm::TimeRecord::getCurrentTime();
  endTime -= startTime;

  if (Verbose)
    errs() << "populateHashTable took: " << endTime.getWallTime() << "s\n";

  // Write the cache to disk.
  startTime = llvm::TimeRecord::getCurrentTime();
  int Size = Generator.writeStatCache(Out);
  endTime = llvm::TimeRecord::getCurrentTime();
  endTime -= startTime;

  if (Verbose)
    errs() << "writeStatCache took: " << endTime.getWallTime() << "s\n";

  // We might have opened a pre-exising cache which was bigger.
  llvm::sys::fs::resize_file(FD, Size);

  return 0;
}
