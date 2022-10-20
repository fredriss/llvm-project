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
#include "llvm/Support/OnDiskHashTable.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Timer.h"
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

#include "GetAttrList.h"

#include <sys/mount.h>
#include <sys/param.h>
#endif // __APPLE__

// The clang-stat-cache utility creates an on-disk cache for the stat data
// of a file-system tree which is expected to be immutable during a build.
//
// The format of the stat cache is (pseudo-code):
//  struct stat_cache {
//    char     Magic[4];       // "STAT" or "Stat"
//    uint32_t BucketOffset;   // See BucketOffset in OnDiskHashTable.h
//    uint64_t ValidityToken;  // Platofrm specific data allowing to check
//                             // whether the cache is up-to-date.
//    char     BaseDir[N];     // Zero terminated path to the base directory
//    < OnDiskHashtable Data > // Data for the has table. The keys are the
//                             // relative paths under BaseDir. The data is
//                             // llvm::sys::fs::file_status structures.
//  };

#define MAGIC_CASE_SENSITIVE "Stat"
#define MAGIC_CASE_INSENSITIVE "STAT"

using namespace llvm;

cl::OptionCategory StatCacheCategory("clang-stat-cache options");

cl::opt<std::string> OutputFilename("o", cl::Required,
                                    cl::desc("Specify output filename"),
                                    cl::value_desc("filename"),
                                    cl::cat(StatCacheCategory));

cl::opt<std::string> TargetDirectory(cl::Positional, cl::Required,
                                     cl::value_desc("dirname"),
                                     cl::cat(StatCacheCategory));

cl::opt<bool> Verbose("v", cl::desc("More verbose output"));

class StatusInfo {
public:
  typedef StringRef key_type; // Must be copy constructible
  typedef StringRef &key_type_ref;
  typedef sys::fs::file_status data_type; // Must be copy constructible
  typedef sys::fs::file_status &data_type_ref;
  typedef uint32_t hash_value_type; // The type the hash function returns.
  typedef uint32_t offset_type;     // The type for offsets into the table.

  /// Calculate the hash for Key
  static hash_value_type ComputeHash(key_type_ref Key) {
    return static_cast<size_t>(hash_value(Key));
  }

  /// Return the lengths, in bytes, of the given Key/Data pair.
  static std::pair<unsigned, unsigned>
  EmitKeyDataLength(raw_ostream &Out, key_type_ref Key, data_type_ref Data) {
    using namespace llvm::support;
    endian::Writer LE(Out, little);
    unsigned KeyLen = Key.size();
    unsigned DataLen = sizeof(Data);
    LE.write<uint16_t>(KeyLen);
    LE.write<uint16_t>(DataLen);
    return std::make_pair(KeyLen, DataLen);
  }

  static void EmitKey(raw_ostream &Out, key_type_ref Key, unsigned KeyLen) {
    Out.write(Key.data(), KeyLen);
  }

  /// Write Data to Out.  DataLen is the length from EmitKeyDataLength.
  static void EmitData(raw_ostream &Out, key_type_ref Key, data_type_ref Data,
                       unsigned Len) {
    Out.write((const char *)&Data, Len);
  }

  static bool EqualKey(key_type_ref Key1, key_type_ref Key2) {
    return Key1 == Key2;
  }
};

namespace {

size_t writeOutHashTable(raw_fd_ostream &Out,
                         OnDiskChainedHashTableGenerator<StatusInfo> &Generator,
                         StringRef BaseDir, uint64_t ValidityToken,
                         bool IsCaseSensitive) {
  // Magic value.
  if (IsCaseSensitive)
    Out.write(MAGIC_CASE_SENSITIVE, 4);
  else
    Out.write(MAGIC_CASE_INSENSITIVE, 4);
  // Placeholder for BucketOffset, filled in below.
  Out.write("\0\0\0\0", 4);
  // Write out the validity token.
  Out.write((const char *)&ValidityToken, sizeof(ValidityToken));
  // Write out the base directory for the cache.
  if (IsCaseSensitive)
    Out.write(BaseDir.data(), BaseDir.size() + 1);
  else
    Out.write(BaseDir.lower().c_str(), BaseDir.size() + 1);
  // Write out the hashtable data.
  uint32_t BucketOffset = Generator.Emit(Out);
  int Size = Out.tell();
  // Move back to right after the Magic to insert BucketOffset
  Out.seek(4);
  Out.write((const char *)&BucketOffset, sizeof(BucketOffset));
  return Size;
}

#if __APPLE__
template <typename... Attrs>
sys::fs::file_status
statFromAttributes(const std::tuple<Attrs...> &Attributes) {
  using namespace llvm::sys::fs;
  static file_type objTypeToLLVM[] = {
      file_type::type_unknown,   file_type::regular_file,
      file_type::directory_file, file_type::block_file,
      file_type::character_file, file_type::symlink_file,
      file_type::socket_file,    file_type::fifo_file,
      file_type::type_unknown,   file_type::type_unknown,
      file_type::type_unknown};
  perms Perms =
      static_cast<perms>(*std::get<AttrCommonAccessMask>(Attributes)) &
      all_perms;

  auto objType = *std::get<AttrCommonObjType>(Attributes);
  assert(objType < sizeof(objTypeToLLVM) &&
         "Invalid objType returned by getattrlist");
  file_status s(objTypeToLLVM[objType], Perms,
                *std::get<AttrCommonDevId>(Attributes),
                objType == VDIR ? *std::get<AttrDirLinkCount>(Attributes)
                                : *std::get<AttrFileLinkCount>(Attributes),
                *std::get<AttrCommonFileId>(Attributes),
                (*std::get<AttrCommonAccTime>(Attributes)).tv_sec,
                (*std::get<AttrCommonAccTime>(Attributes)).tv_nsec,
                (*std::get<AttrCommonModTime>(Attributes)).tv_sec,
                (*std::get<AttrCommonModTime>(Attributes)).tv_nsec,
                *std::get<AttrCommonOwnerId>(Attributes),
                *std::get<AttrCommonGrpId>(Attributes),
                objType == VDIR ? *std::get<AttrDirDataLength>(Attributes)
                                : *std::get<AttrFileDataLength>(Attributes));
  return s;
}

void populateHashTable(StringRef BasePath,
                       OnDiskChainedHashTableGenerator<StatusInfo> &Generator,
                       std::list<std::string> &Filenames,
                       bool IsCaseSensitive) {
  using namespace llvm::sys::fs;

  Filenames.emplace_front(BasePath);
  std::list<StringRef> DirStack = {Filenames.front()};

  while (true) {
    if (DirStack.empty())
      break;

    StringRef CurrentDirName = DirStack.front();
    DirStack.pop_front();

    GetAttrListBulk<AttrCommonReturnedAttrs, AttrCommonName, AttrCommonDevId,
                    AttrCommonObjType, AttrCommonModTime, AttrCommonAccTime,
                    AttrCommonOwnerId, AttrCommonGrpId, AttrCommonAccessMask,
                    AttrCommonFileId, AttrDirLinkCount, AttrDirDataLength,
                    AttrFileLinkCount, AttrFileDataLength>
        Entries(CurrentDirName.str());

    auto Result = Entries.execute();

    if (auto E = Result.takeError()) {
      errs() << "getattrlistbulk failed on '" << CurrentDirName << "': " << E
             << '\n';
      continue;
    }

    for (const auto &attrs : *Result) {
      sys::fs::file_status s;

      std::string filename = *std::get<AttrCommonName>(attrs);
      if (!IsCaseSensitive)
        filename = StringRef(filename).lower();
      std::string name =
          (CurrentDirName + sys::path::get_separator() + filename).str();
      Filenames.emplace_back(std::move(name));

      auto objType = *std::get<AttrCommonObjType>(attrs);

      // getattrlistbulk doesn't have a mode to follow links by default.
      // When encountering a link, use the normal stat to resolve it.
      if (objType == VLNK)
        sys::fs::status(Filenames.back(), s);
      else
        s = statFromAttributes(attrs);

      // If the link points to a directory, add it to our queue.
      if (s.type() == file_type::directory_file)
        DirStack.emplace_back(Filenames.back());

      StringRef Path(Filenames.back());
      __attribute__((unused)) bool consumed = Path.consume_front(BasePath);
      assert(consumed && "Path does not start with expected prefix.");

      Generator.insert(Path, s);
    }
  }
}

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

std::error_code
populateHashTable(StringRef BasePath,
                  OnDiskChainedHashTableGenerator<StatusInfo> &Generator,
                  std::list<std::string> &Filenames, bool IsCaseSensitive) {
  using namespace llvm;
  using namespace sys::fs;

  std::error_code ErrorCode;

  for (recursive_directory_iterator I(BasePath, ErrorCode), E;
       I != E && !ErrorCode; I.increment(ErrorCode)) {

    std::string name = I->path();
    if (!IsCaseSensitive)
      name = StringRef(name).lower();

    Filenames.emplace_back(std::move(name));
    StringRef Path(Filenames.back().c_str());

    sys::fs::file_status s;
    // This can fail (broken symlink) and leave the file_status with
    // its default values. The reader knows this.
    status(Path, s);

    __attribute__((unused)) bool consumed = Path.consume_front(BasePath);
    assert(consumed && "Path does not start with expected prefix.");

    Generator.insert(Path, s);
  }

  return ErrorCode;
}

bool checkForFilesystemUpdates(uint64_t &ValidityToken) { return true; }

#endif // __APPLE__

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

  struct StatCacheHeader {
    char Magic[4];
    uint32_t BucketOffset;
    uint64_t ValidityToken;
    char BaseDir[1];
  };

  if (Size < sizeof(StatCacheHeader)) {
    llvm::errs() << "The output cache file is not empty or a valid cache. "
                 << "Giving up.\n";
    exit(1);
  }

  auto ErrorOrBuffer =
      MemoryBuffer::getOpenFile(FD, OutputFilename, Status.getSize());
  auto *Header = reinterpret_cast<const StatCacheHeader *>(
      (*ErrorOrBuffer)->getBufferStart());

  if ((memcmp(Header->Magic, MAGIC_CASE_INSENSITIVE, sizeof(Header->Magic)) &&
       memcmp(Header->Magic, MAGIC_CASE_SENSITIVE, sizeof(Header->Magic))) ||
      Header->BucketOffset > Status.getSize()) {
    llvm::errs() << "The output cache file is not empty or a valid cache. "
                 << "Giving up.\n";
    exit(1);
  }

  auto PathLen = strnlen(Header->BaseDir, Status.getSize() - 16);

  if (Header->BaseDir[PathLen] != 0) {
    // Invalid base directory, but we have a valid magic number. Rewrite
    // this cache.
    return false;
  }

  if (StringRef(Header->BaseDir) != TargetDirectory) {
    llvm::errs() << "Exisitng cache has different directory. Regenerating...\n";
    return false;
  }

  ValidityToken = Header->ValidityToken;
  return checkForFilesystemUpdates(ValidityToken);
}

} // namespace

int main(int argc, char *argv[]) {
  cl::ParseCommandLineOptions(argc, argv);

  OnDiskChainedHashTableGenerator<StatusInfo> Generator;
  std::list<std::string> Paths;

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
  EC = sys::fs::is_local(TargetDirectory, IsLocal);
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

  auto startTime = llvm::TimeRecord::getCurrentTime();
  populateHashTable(TargetDirectory, Generator, Paths, IsCaseSensitive);
  auto endTime = llvm::TimeRecord::getCurrentTime();
  endTime -= startTime;

  if (Verbose)
    errs() << "populateHashTable took: " << endTime.getWallTime() << "s\n";

  startTime = llvm::TimeRecord::getCurrentTime();
  int Size = writeOutHashTable(Out, Generator, TargetDirectory, ValidityToken,
                               IsCaseSensitive);
  endTime = llvm::TimeRecord::getCurrentTime();
  endTime -= startTime;

  if (Verbose)
    errs() << "writeOutHashTable took: " << endTime.getWallTime() << "s\n";

  // We might have opened a pre-exising cache which was bigger.
  llvm::sys::fs::resize_file(FD, Size);

  return 0;
}
