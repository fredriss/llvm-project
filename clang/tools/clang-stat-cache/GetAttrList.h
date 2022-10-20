//===- GetAttrList.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declare a C++ type safe wrapper around Darwin's getattrlistbulk(2)
// function. `getattrlistbulk` allows gathering of `stat` data for full
// directories significantly faster than iterating the files one by one.
//
// The API works by specifying a list of attributes you want to gather for
// every file in a directory. The list is not fixed, and thus it can gather
// more or less data than what `stat` does.
//
// It's very easy to misuse it or to silently break one of tits usage
// constraints:
//   - `getattrlistbulk` (as opposed to `getattrlist`) requires that
//     ATTR_CMN_RETURNED_ATTRS is used.
//   - The attributes needs to be extracted in the correct order from the
//     result buffer. There is no way to check that you are extracting the
//     correct attribute at the correct offset.
//
// This wrapper aims to make these mistakes compile time errors.
//
//===----------------------------------------------------------------------===//

#ifndef CLANG_STAT_CACHE_GETATTRLIST_H
#define CLANG_STAT_CACHE_GETATTRLIST_H

#include <llvm/ADT/Optional.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/attr.h>
#include <sys/kauth.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <unistd.h>

template <typename... Attrs> struct AttrList;

/// @name Attribute definition
/// @{

/// Data about an attribute that `getattrlist{,bulk}` can request.
///  - Type is the type of the data stored in the result buffer.
///  - Mask is the mask value that represents this attribute.
///  - Ord is a value that globally orders attributes.
///  - AttrGroup is a member pointer into `struct attrlist` to
///    the field used to request the attribute.
///  - ReturnedAttrGroup is a member pointer into `attribute_set_t`
///    to the field used to check for attribut availability.
template <typename Type, unsigned long Mask, int Ord,
          uint32_t attrlist::*AttrGroup,
          uint32_t attribute_set_t::*ReturnedAttrGroup>
struct AttributeData {
  static const int Ordinal = Ord;

  static void applyAttrListMask(attrlist &list) { list.*AttrGroup |= Mask; }
  static bool checkReturned(const attribute_set_t &set) {
    return set.*ReturnedAttrGroup & Mask;
  }

  // Make an AttributeData behave as its stored attribute.
  const Type &operator*() const { return *Value; }
  operator bool() const { return Value.hasValue(); }

protected:
  template <typename... Attrs> friend struct AttrList;
  // A requested attribute is not guaranteed to be returned, hence the
  // use of an Optional.
  llvm::Optional<Type> Value;
};

/// Represents an attribute with a generic `extract` method. This was
/// not folded into AttributeData to allow specialization of extraction
/// based on the attribute Type.
template <typename Type, unsigned long Mask, int Ord,
          uint32_t attrlist::*AttrGroup,
          uint32_t attribute_set_t::*ReturnedAttrGroup>
struct Attribute
    : AttributeData<Type, Mask, Ord, AttrGroup, ReturnedAttrGroup> {
  using AttributeType = Type;

  void extract(char *&Data, const llvm::Optional<attribute_set_t> &Returned) {
    using namespace llvm::support;
    if (!Returned || this->checkReturned(*Returned)) {
      // Extract the attribute if we didn't ask for ATTR_CMN_RETURNED_ATTRS
      // or if we got an `attribute_set_t` and the attribute bit is set.
      Type Val;
      memcpy(&Val, Data, sizeof(Type));
      this->Value = Val;
      Data += sizeof(Type);
    } else {
      // Otherwise make the Optional invalid.
      this->Value.reset();
    }
  }
};

/// Specialized Attribute which handles string data.
template <unsigned long Mask, int Ord, uint32_t attrlist::*AttrGroup,
          uint32_t attribute_set_t::*ReturnedAttrGroup>
struct Attribute<std::string, Mask, Ord, AttrGroup, ReturnedAttrGroup>
    : AttributeData<std::string, Mask, Ord, AttrGroup, ReturnedAttrGroup> {
  using AttributeType = std::string;

  void extract(char *&Data, const llvm::Optional<attribute_set_t> &Returned) {
    if (!Returned || this->checkReturned(*Returned)) {
      // Special handling for variable length data applied to strings.
      attrreference_t StringData = *(attrreference_t *)Data;
      this->Value = std::string(Data + StringData.attr_dataoffset);
      Data += sizeof(attrreference_t);
    } else {
      this->Value.reset();
    }
  }
};

template <unsigned long Mask, int Ord, uint32_t attrlist::*AttrGroup,
          uint32_t attribute_set_t::*ReturnedAttrGroup>
struct Attribute<attribute_set_t, Mask, Ord, AttrGroup, ReturnedAttrGroup>
    : AttributeData<attribute_set_t, Mask, Ord, AttrGroup, ReturnedAttrGroup> {
  using AttributeType = attribute_set_t;

  void extract(char *&Data, const llvm::Optional<attribute_set_t> &Returned) {
    // When ATTR_CMN_RETURNED_ATTRS is requested, it is always uncoditionally
    // provided (and always first). Do not check `returned`.
    this->Value = *(attribute_set_t *)Data;
    Data += sizeof(attribute_set_t);
  }
};

/// @}
/// @name Attribute categories
/// @{

template <typename Type, unsigned long Mask, int Ordinal>
using CommonAttribute = Attribute<Type, Mask, Ordinal, &attrlist::commonattr,
                                  &attribute_set_t::commonattr>;

template <typename Type, unsigned long Mask, int Ordinal>
using DirAttribute = Attribute<Type, Mask, Ordinal, &attrlist::dirattr,
                               &attribute_set_t::dirattr>;

template <typename Type, unsigned long Mask, int Ordinal>
using FileAttribute = Attribute<Type, Mask, Ordinal, &attrlist::fileattr,
                                &attribute_set_t::fileattr>;

/// @}
/// @name Attribute definitions
/// @{

#define DECLARE_COMMON_ATTRIBUTE(name, value, type)                            \
  using name = CommonAttribute<type, value, __LINE__>

#define DECLARE_DIR_ATTRIBUTE(name, value, type)                               \
  using name = DirAttribute<type, value, __LINE__>

#define DECLARE_FILE_ATTRIBUTE(name, value, type)                              \
  using name = FileAttribute<type, value, __LINE__>

// This is a complete list of the common attributes, but the lists of Dir and
// File attributes only contain the ones we use.
DECLARE_COMMON_ATTRIBUTE(AttrCommonReturnedAttrs, ATTR_CMN_RETURNED_ATTRS,
                         attribute_set_t);
DECLARE_COMMON_ATTRIBUTE(AttrCommonName, ATTR_CMN_NAME, std::string);
DECLARE_COMMON_ATTRIBUTE(AttrCommonDevId, ATTR_CMN_DEVID, dev_t);
DECLARE_COMMON_ATTRIBUTE(AttrCommonFsId, ATTR_CMN_FSID, fsid_t);
DECLARE_COMMON_ATTRIBUTE(AttrCommonObjType, ATTR_CMN_OBJTYPE, fsobj_type_t);
DECLARE_COMMON_ATTRIBUTE(AttrCommonObjTag, ATTR_CMN_OBJTAG, fsobj_tag_t);
DECLARE_COMMON_ATTRIBUTE(AttrCommonObjId, ATTR_CMN_OBJID, fsobj_id_t);
DECLARE_COMMON_ATTRIBUTE(AttrCommonObjPermanentId, ATTR_CMN_OBJPERMANENTID,
                         fsobj_id_t);
DECLARE_COMMON_ATTRIBUTE(AttrCommonParObjId, ATTR_CMN_PAROBJID, fsobj_id_t);
DECLARE_COMMON_ATTRIBUTE(AttrCommonScript, ATTR_CMN_SCRIPT, text_encoding_t);
DECLARE_COMMON_ATTRIBUTE(AttrCommonCrTime, ATTR_CMN_CRTIME, timespec);
DECLARE_COMMON_ATTRIBUTE(AttrCommonModTime, ATTR_CMN_MODTIME, timespec);
DECLARE_COMMON_ATTRIBUTE(AttrCommonChgTime, ATTR_CMN_CHGTIME, timespec);
DECLARE_COMMON_ATTRIBUTE(AttrCommonAccTime, ATTR_CMN_ACCTIME, timespec);
DECLARE_COMMON_ATTRIBUTE(AttrCommonBkupTime, ATTR_CMN_BKUPTIME, timespec);
DECLARE_COMMON_ATTRIBUTE(AttrCommonFndrInfo, ATTR_CMN_FNDRINFO, char[32]);
DECLARE_COMMON_ATTRIBUTE(AttrCommonOwnerId, ATTR_CMN_OWNERID, uid_t);
DECLARE_COMMON_ATTRIBUTE(AttrCommonGrpId, ATTR_CMN_GRPID, gid_t);
DECLARE_COMMON_ATTRIBUTE(AttrCommonAccessMask, ATTR_CMN_ACCESSMASK, uint32_t);
DECLARE_COMMON_ATTRIBUTE(AttrCommonFlags, ATTR_CMN_FLAGS, uint32_t);
DECLARE_COMMON_ATTRIBUTE(AttrCommonGenCount, ATTR_CMN_GEN_COUNT, uint32_t);
DECLARE_COMMON_ATTRIBUTE(AttrCommonDocumentId, ATTR_CMN_DOCUMENT_ID, uint32_t);
DECLARE_COMMON_ATTRIBUTE(AttrCommonUserAccess, ATTR_CMN_USERACCESS, uint32_t);
// DECLARE_COMMON_ATTRIBUTE(, ATTR_CMN_EXTENDED_SECURITY, variable lenght
// kauth);
DECLARE_COMMON_ATTRIBUTE(AttrCommonUuid, ATTR_CMN_UUID, guid_t);
DECLARE_COMMON_ATTRIBUTE(AttrCommonGrpUuid, ATTR_CMN_GRPUUID, guid_t);
DECLARE_COMMON_ATTRIBUTE(AttrCommonFileId, ATTR_CMN_FILEID, uint64_t);
DECLARE_COMMON_ATTRIBUTE(AttrCommonParentId, ATTR_CMN_PARENTID, uint64_t);
DECLARE_COMMON_ATTRIBUTE(AttrCommonFullPath, ATTR_CMN_FULLPATH, std::string);
DECLARE_COMMON_ATTRIBUTE(AttrCommonAddedTime, ATTR_CMN_ADDEDTIME, timespec);
DECLARE_COMMON_ATTRIBUTE(AttrCommonProtectFlags, ATTR_CMN_DATA_PROTECT_FLAGS,
                         uint32_t);

DECLARE_DIR_ATTRIBUTE(AttrDirLinkCount, ATTR_DIR_LINKCOUNT, uint32_t);
DECLARE_DIR_ATTRIBUTE(AttrDirDataLength, ATTR_DIR_DATALENGTH, off_t);

DECLARE_FILE_ATTRIBUTE(AttrFileLinkCount, ATTR_FILE_LINKCOUNT, uint32_t);
DECLARE_FILE_ATTRIBUTE(AttrFileDataLength, ATTR_FILE_DATALENGTH, off_t);

/// @}
/// @name Attribute lists
/// @{

namespace detail {
// Utility to apply a functor to all std::tuple elements.
template <typename T, typename F, int... Is>
constexpr void for_each(T &&Tuple, F Functor,
                        std::integer_sequence<int, Is...>) {
  auto L = {(Functor(std::get<Is>(Tuple)), 0)...};
  (void)L;
}

// Functor to use with for_each to apply the bit masks of all the attributes
// in a list to the provided `struct attrlist`.
struct ApplyAttrListMaskFunctor {
  attrlist &List;

  ApplyAttrListMaskFunctor(attrlist &List) : List(List) {}

  template <typename T> void operator()(T const &) {
    T::applyAttrListMask(List);
  }
};

// Functor to use with for_each to extract all the attributes from the passed
// data pointer according to the information in the `returned` attribute_set_t.
struct ExtractAttrFunctor {
  char *&Data;
  const llvm::Optional<attribute_set_t> &Returned;

  ExtractAttrFunctor(char *&Data,
                     const llvm::Optional<attribute_set_t> &Returned)
      : Data(Data), Returned(Returned) {}

  template <typename T> void operator()(T &V) { V.extract(Data, Returned); }
};

// Utility to check that the Ordinals of attributes in a list are only
// increasing.
template <typename... Attrs> struct CheckOrdinals;

template <typename Attr, typename... Attrs>
struct CheckOrdinals<Attr, Attrs...> {
  static const int Ordinal = Attr::Ordinal;

  static_assert(Attr::Ordinal < CheckOrdinals<Attrs...>::Ordinal,
                "Attributes passed in the wrong order");
};

template <typename Attr> struct CheckOrdinals<Attr> {
  static const int Ordinal = Attr::Ordinal;
};
} // namespace detail

/// Attribute list implementation. We store the attributes in a `std::tuple`.
template <typename... Attrs>
struct AttrListImpl : private detail::CheckOrdinals<Attrs...> {
  std::tuple<Attrs...> Storage;

  void extract(char *&Data, const llvm::Optional<attribute_set_t> &Returned) {
    detail::for_each(Storage, detail::ExtractAttrFunctor(Data, Returned),
                     std::make_integer_sequence<int, sizeof...(Attrs)>());
  }

  void applyAttrListMask(attrlist &List) {
    detail::for_each(Storage, detail::ApplyAttrListMaskFunctor(List),
                     std::make_integer_sequence<int, sizeof...(Attrs)>());
  }
};

/// Generic Attribute list definition.
template <typename... Attrs> struct AttrList : AttrListImpl<Attrs...> {
  const llvm::Optional<attribute_set_t> &getReturned() {
    static const llvm::Optional<attribute_set_t> Returned = {};
    return Returned;
  }

  static const bool HasReturnedAttrs = false;
};

/// Attribute list partial specialization to specialize the behavior when
/// ATTR_CMN_RETURNED_ATTRS is passed. ATTR_CMN_RETURNED_ATTRS Can only be first
/// in the list.
template <typename... Attrs>
struct AttrList<AttrCommonReturnedAttrs, Attrs...>
    : AttrListImpl<AttrCommonReturnedAttrs, Attrs...> {
  const llvm::Optional<attribute_set_t> &getReturned() {
    return std::get<0>(this->Storage).Value;
  }

  static const bool HasReturnedAttrs = true;
};

/// @}
/// @name Getattrlistbulk wrapper.
/// @{

/// Getattrlistbulk wrapper taking the list of attributes to query as a list
/// of template parameters.
template <typename... Attrs> class GetAttrListBulk {
  // Target directory.
  std::string DirPath;

  // Our attributes definitions and storage.
  AttrList<Attrs...> Attributes;

  // List of requested attributes.
  attrlist RequestedAttrs = {};

  // Result buffer.
  size_t AttrBufSize = 0;
  std::unique_ptr<char[]> AttrBuf;

  static_assert(AttrList<Attrs...>::HasReturnedAttrs,
                "GetAttrListBulk works only in the presence of "
                "AttrCommonReturnedAttrs.");

public:
  /// Create a `getattrlistbulk` call for the \a path directory,
  /// using a result buffer of size \a bufSize.
  GetAttrListBulk(const std::string &DirPath, size_t BufSize = 4096)
      : DirPath(DirPath), AttrBufSize(BufSize), AttrBuf(new char[BufSize]) {
    RequestedAttrs.bitmapcount = ATTR_BIT_MAP_COUNT;
    Attributes.applyAttrListMask(RequestedAttrs);
  };

  llvm::Expected<std::vector<std::tuple<Attrs...>>> execute() {
    int DirFD = open(DirPath.c_str(), O_RDONLY, 0);

    if (DirFD == -1)
      return llvm::errorCodeToError(
          std::error_code(errno, std::system_category()));

    std::vector<std::tuple<Attrs...>> Result;

    // getattrlistbulk return value. On a successful call, this is the number
    // of entries the call populated in the result buffer.
    int RetCount = 0;
    // Index of the attribute set being extracted in the current batch.
    // (Goes from 0 to RetCount-1);
    int CurAttrIndex = 0;
    // A pointer into the result buffer pointing at the data for attribute
    // to be decoded by the next iteration;
    char *NextAttr = nullptr;

    while (true) {

      // Initial condition (CurAttrIndex == RetCount == 0) and also condition
      // after having iterated over all the elements returned by the last call.
      if (CurAttrIndex == RetCount) {
        CurAttrIndex = 0;
        RetCount = ::getattrlistbulk(DirFD, &RequestedAttrs, AttrBuf.get(),
                                     AttrBufSize, 0);

        // If the call fails, even when it's not the first one, return an error
        // and no results.
        if (RetCount == -1)
          return llvm::errorCodeToError(
              std::error_code(errno, std::system_category()));

        // End of iteration for the current call.
        if (RetCount == 0)
          break;

        NextAttr = AttrBuf.get();
      }

      // From the `getattrlist` manpage:
      // "The first element of the [attribute] is a u_int32_t that contains the
      // overall length, in bytes, of the attributes returned.  This size
      // includes the length field itself."
      char *CurAttr = NextAttr;
      using namespace llvm::support;
      int Length = *(uint32_t *)CurAttr;
      // Store the start of the next attribute.
      NextAttr = CurAttr + Length;
      CurAttr += 4;

      // Extract the actual returned attributes.
      Attributes.extract(CurAttr, Attributes.getReturned());

      Result.emplace_back(std::move(Attributes.Storage));
      CurAttrIndex++;
    }

    ::close(DirFD);
    return Result;
  }
};

/// @}

#endif // CLANG_STAT_CACHE_GETATTRLIST_H
