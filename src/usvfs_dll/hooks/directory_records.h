#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <ntdll_declarations.h>

// The 8.3 name of a record: a CCHAR byte count and a WCHAR[12] array.
struct ShortNameFields
{
  ULONG lengthOffset;
  ULONG offset;
};

// The records one directory information class returns.
struct DirectoryRecordLayout
{
  FILE_INFORMATION_CLASS informationClass;
  // the size of a record with a one-character name; ntdll rejects shorter buffers
  ULONG minimumLength;
  // the alignment of a record, which x64 ntdll requires of the buffer
  ULONG alignment;
  ULONG fileNameLengthOffset;
  ULONG fileNameOffset;
  // only the classes with "Both" in their name carry an 8.3 name
  std::optional<ShortNameFields> shortName;
};

// Returns the layout of the records of a directory information class whose records
// carry a FileName, the only ones the virtual listing can filter and rename.
std::optional<DirectoryRecordLayout>
findDirectoryRecordLayout(FILE_INFORMATION_CLASS informationClass);

// A record that a directory query returned, read where it lies. The bytes run from
// the start of the record to the end of the data the query returned, so that a
// record cut short by the end of the buffer reads as truncated instead of past it.
class DirectoryRecord
{
public:
  DirectoryRecord(const DirectoryRecordLayout& layout,
                  std::span<const std::byte> bytes);

  // the offset of the next record, 0 for the last one
  ULONG nextEntryOffset() const;
  // the size of the record without the padding up to the next one
  ULONG size() const;
  // the full length of the name in bytes, also for a record cut short
  ULONG fileNameLength() const;
  // whether the record runs past the bytes, as the partial record that comes with
  // STATUS_BUFFER_OVERFLOW does
  bool isTruncated() const;
  // The name, or as much of it as the bytes hold. Copied rather than viewed in
  // place: WOW64 accepts the buffer of a legacy query at any address, so the name
  // need not be aligned for WCHAR.
  std::wstring fileName() const;
  // The name in place, or as much of it as the bytes hold. Only for a record that lies
  // aligned for WCHAR, as one in a buffer of usvfs's own does.
  std::wstring_view fileNameView() const;
  // the bytes of the record without the padding, or as many of them as there are
  std::span<const std::byte> bytes() const;

private:
  // the bytes of the name, or as many of them as there are
  std::span<const std::byte> fileNameBytes() const;

  // reads 0 for a field past the bytes
  ULONG readField(ULONG offset) const;

  DirectoryRecordLayout m_layout;
  std::span<const std::byte> m_bytes;
};

// The buffer of a directory query, filled with records the way ntdll fills it:
// each record starts a multiple of 8 bytes from the start of the buffer, the
// NextEntryOffset of the one before links to it, and the size reported ends with the
// last record, without padding after it.
class DirectoryOutput
{
public:
  // How the query the output answers asks for its entries.
  struct Options
  {
    bool isSingleEntry;
    // an empty result is STATUS_NO_SUCH_FILE on the first call only
    bool isFirstCall;
  };

  DirectoryOutput(std::span<std::byte> buffer, const DirectoryRecordLayout& layout,
                  Options options);

  bool isEmpty() const;
  // whether the query wants no further record: it asked for a single one and has it
  bool isSatisfied() const;
  // Whether the output holds only the partial record of an entry that does not fit
  // it, which the query reports with STATUS_BUFFER_OVERFLOW.
  bool isOverflowed() const;
  // the bytes written, up to the end of the last record
  ULONG size() const;

  // Writes a record as it is. Returns false, and writes nothing, when the record does
  // not fit, is truncated, or the query is satisfied. A record that does not fit an
  // empty output is the exception: like ntdll, it writes as much of it as fits, with
  // the full FileNameLength, and still returns false.
  bool append(const DirectoryRecord& record);
  // Writes a record of another directory under its name here, on the same terms as
  // append, and without an 8.3 name.
  bool appendRenamed(const DirectoryRecord& record, std::wstring_view name);
  // Writes the data of a native read that ended with STATUS_BUFFER_OVERFLOW, its
  // partial record, as it is into an empty output that it fits.
  void mirror(std::span<const std::byte> native);

  // Turns the status a fill ended with into the result of the query, and writes the
  // status block unless the result is an error, which ntdll reports without it.
  NTSTATUS complete(NTSTATUS fillStatus, IO_STATUS_BLOCK& statusBlock) const;

private:
  // Makes room for a record after the last one and links the last one to it. A record
  // that does not fit an empty output gets the whole buffer, which the caller fills
  // with as much of the record as fits. Returns nothing when the record does not fit
  // or the query is satisfied.
  std::optional<std::span<std::byte>> place(size_t recordSize);

  std::span<std::byte> m_buffer;
  DirectoryRecordLayout m_layout;
  Options m_options;
  // where the last record starts; nothing while the output is empty
  std::optional<size_t> m_lastRecordOffset;
  size_t m_size       = 0;
  bool m_isOverflowed = false;
};
