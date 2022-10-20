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
struct CallbackInfo {
  FSEventStreamEventId LastEvent;
  bool SeenChanges = false;
};

void FSEventsCallback(ConstFSEventStreamRef streamRef, void *CtxInfo,
                      size_t numEvents, void *eventPaths,
                      const FSEventStreamEventFlags *eventFlags,
                      const FSEventStreamEventId *eventIds) {
  CallbackInfo *Info = static_cast<CallbackInfo *>(CtxInfo);
  for (size_t i = 0; i < numEvents; ++i) {
    Info->LastEvent = eventIds[i];
    if (eventFlags[i] == 0x10) {
      CFRunLoopStop(CFRunLoopGetCurrent());
      break;
    }
    Info->SeenChanges = true;
  }
}

bool checkForFilesystemUpdates(uint64_t &ValidityToken) {
  CFStringRef TargetDir = CFStringCreateWithCStringNoCopy(
      kCFAllocatorDefault, TargetDirectory.c_str(), kCFStringEncodingASCII,
      kCFAllocatorNull);
  CFArrayRef PathsToWatch =
      CFArrayCreate(nullptr, (const void **)&TargetDir, 1, nullptr);
  CallbackInfo Info;
  FSEventStreamContext Ctx = {0, &Info, nullptr, nullptr, nullptr};
  FSEventStreamRef Stream;
  CFAbsoluteTime Latency = 0; /* Latency in seconds */

  FSEventStreamEventId StartEvent = ValidityToken;

  /* Create the stream, passing in a callback */
  Stream = FSEventStreamCreate(
      NULL,
      &FSEventsCallback,                    // Callback function
      &Ctx, PathsToWatch, StartEvent,       // Or a previous event ID
      Latency, kFSEventStreamCreateFlagNone // Flags explained in reference
  );

  FSEventStreamSetDispatchQueue(Stream, dispatch_get_main_queue());
  if (!FSEventStreamStart(Stream)) {
    errs() << "Failed to create FS event stream. "
           << "Considering the cache up-to-date.\n";
    return true;
  }

  CFRunLoopRun();
  ValidityToken = Info.LastEvent;
  return !Info.SeenChanges;
}

#else // __APPLE__

bool checkForFilesystemUpdates(uint64_t &ValidityToken) { return true; }

#endif // __APPLE__

std::error_code
populateHashTable(StringRef BasePath,
                  StatCacheFileSystem::StatCacheWriter &Generator) {
  using namespace llvm;
  using namespace sys::fs;

  std::error_code ErrorCode;

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

bool checkValidity(int FD, uint64_t &ValidityToken) {
  sys::fs::file_status Status;
  auto EC = sys::fs::status(FD, Status);
  if (EC) {
    llvm::errs() << "fstat failed\n";
    return false;
  }

  auto Size = Status.getSize();
  if (Size == 0) {
#ifdef __APPLE__
    ValidityToken = FSEventsGetCurrentEventId();
#endif
    // New file.
    return false;
  }

  auto ErrorOrBuffer =
      MemoryBuffer::getOpenFile(FD, OutputFilename, Status.getSize());

  StringRef BaseDir;
  if (auto E = StatCacheFileSystem::validateCacheFile(
          (*ErrorOrBuffer)->getMemBufferRef(), BaseDir, ValidityToken)) {
    llvm::errs() << "The output cache file exists and is not a valid stat "
                    "cache. Aborting."
                 << '\n';
    exit(1);
  }

  if (BaseDir != TargetDirectory) {
    llvm::errs() << "Exisitng cache has different directory. Regenerating...\n";
    return false;
  }

  return checkForFilesystemUpdates(ValidityToken);
}

} // namespace

int main(int argc, char *argv[]) {
  cl::ParseCommandLineOptions(argc, argv);

  StringRef Dirname(TargetDirectory);

  std::error_code EC;
  int FD;
  EC = sys::fs::openFileForReadWrite(
      OutputFilename, FD, llvm::sys::fs::CD_OpenAlways, llvm::sys::fs::OF_None);
  if (EC) {
    llvm::errs() << llvm::createFileError(
                        "Failed to open '" + OutputFilename + "'", EC)
                 << "\n";
    return 1;
  }

  raw_fd_ostream Out(FD, /* ShouldClose=*/true);

  uint64_t ValidityToken = 0;
  if (checkValidity(FD, ValidityToken)) {
    if (Verbose)
      outs() << "Cache up-to-date, exiting\n";
    return 0;
  }

  if (Verbose)
    outs() << "Building a stat cache for '" << TargetDirectory << "' into '"
           << OutputFilename << "'\n";

  while (!Dirname.empty() && sys::path::is_separator(Dirname.back()))
    Dirname = Dirname.drop_back();

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

  bool IsCaseSensitive = true;
#ifdef _PC_CASE_SENSITIVE
  IsCaseSensitive =
      ::pathconf(TargetDirectory.c_str(), _PC_CASE_SENSITIVE) == 1;
#endif
  StatCacheFileSystem::StatCacheWriter Generator(Dirname, IsCaseSensitive,
                                                 ValidityToken);

  auto startTime = llvm::TimeRecord::getCurrentTime();
  populateHashTable(Dirname, Generator);
  auto endTime = llvm::TimeRecord::getCurrentTime();
  endTime -= startTime;

  if (Verbose)
    errs() << "populateHashTable took: " << endTime.getWallTime() << "s\n";

  startTime = llvm::TimeRecord::getCurrentTime();
  int Size = Generator.writeStatCache(Out);
  endTime = llvm::TimeRecord::getCurrentTime();
  endTime -= startTime;

  if (Verbose)
    errs() << "writeOutHashTable took: " << endTime.getWallTime() << "s\n";

  // We might have opened a pre-exising cache which was bigger.
  llvm::sys::fs::resize_file(FD, Size);

  return 0;
}
