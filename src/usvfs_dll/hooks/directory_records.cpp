#include "directory_records.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

namespace
{

template <typename Record>
constexpr std::optional<ShortNameFields> shortNameFieldsOf()
{
  if constexpr (requires(Record record) { record.ShortName; }) {
    return ShortNameFields{.lengthOffset = offsetof(Record, ShortNameLength),
                           .offset       = offsetof(Record, ShortName)};
  } else {
    return std::nullopt;
  }
}

template <typename Record>
constexpr DirectoryRecordLayout layoutOf(FILE_INFORMATION_CLASS informationClass)
{
  return {.informationClass     = informationClass,
          .minimumLength        = sizeof(Record),
          .alignment            = alignof(Record),
          .fileNameLengthOffset = offsetof(Record, FileNameLength),
          .fileNameOffset       = offsetof(Record, FileName),
          .shortName            = shortNameFieldsOf<Record>()};
}

// The minimum lengths and alignments are those BuildQueryDirectoryIrp in ntoskrnl
// checks the buffer against: it keeps its own tables, which match these records.
constexpr std::array DIRECTORY_RECORD_LAYOUTS{
    layoutOf<FILE_DIRECTORY_INFORMATION>(FileDirectoryInformation),
    layoutOf<FILE_FULL_DIR_INFORMATION>(FileFullDirectoryInformation),
    layoutOf<FILE_BOTH_DIR_INFORMATION>(FileBothDirectoryInformation),
    layoutOf<FILE_NAMES_INFORMATION>(FileNamesInformation),
    layoutOf<FILE_ID_BOTH_DIR_INFORMATION>(FileIdBothDirectoryInformation),
    layoutOf<FILE_ID_FULL_DIR_INFORMATION>(FileIdFullDirectoryInformation),
    layoutOf<FILE_ID_EXTD_DIR_INFORMATION>(FileIdExtdDirectoryInformation),
    layoutOf<FILE_ID_EXTD_BOTH_DIR_INFORMATION>(FileIdExtdBothDirectoryInformation),
    layoutOf<FILE_ID_64_EXTD_DIR_INFORMATION>(FileId64ExtdDirectoryInformation),
    layoutOf<FILE_ID_64_EXTD_BOTH_DIR_INFORMATION>(
        FileId64ExtdBothDirectoryInformation),
    layoutOf<FILE_ID_ALL_EXTD_DIR_INFORMATION>(FileIdAllExtdDirectoryInformation),
    layoutOf<FILE_ID_ALL_EXTD_BOTH_DIR_INFORMATION>(
        FileIdAllExtdBothDirectoryInformation),
};

}  // namespace

std::optional<DirectoryRecordLayout>
findDirectoryRecordLayout(FILE_INFORMATION_CLASS informationClass)
{
  const auto layout = std::ranges::find(DIRECTORY_RECORD_LAYOUTS, informationClass,
                                        &DirectoryRecordLayout::informationClass);
  if (layout == DIRECTORY_RECORD_LAYOUTS.end()) {
    return std::nullopt;
  }
  return *layout;
}

DirectoryRecord::DirectoryRecord(const DirectoryRecordLayout& layout,
                                 std::span<const std::byte> bytes)
    : m_layout(layout), m_bytes(bytes)
{}

ULONG DirectoryRecord::nextEntryOffset() const
{
  // the first field of every directory record
  return readField(0);
}

ULONG DirectoryRecord::size() const
{
  return m_layout.fileNameOffset + fileNameLength();
}

ULONG DirectoryRecord::fileNameLength() const
{
  return readField(m_layout.fileNameLengthOffset);
}

bool DirectoryRecord::isTruncated() const
{
  return size() > m_bytes.size();
}

std::wstring DirectoryRecord::fileName() const
{
  const std::span<const std::byte> bytes = fileNameBytes();
  std::wstring name(bytes.size() / sizeof(WCHAR), L'\0');
  std::memcpy(name.data(), bytes.data(), name.size() * sizeof(WCHAR));
  return name;
}

std::wstring_view DirectoryRecord::fileNameView() const
{
  const std::span<const std::byte> bytes = fileNameBytes();
  assert(reinterpret_cast<std::uintptr_t>(bytes.data()) % alignof(WCHAR) == 0);
  return {reinterpret_cast<const WCHAR*>(bytes.data()), bytes.size() / sizeof(WCHAR)};
}

std::span<const std::byte> DirectoryRecord::fileNameBytes() const
{
  const size_t end = std::min<size_t>(size(), m_bytes.size());
  if (end <= m_layout.fileNameOffset) {
    return {};
  }
  return m_bytes.subspan(m_layout.fileNameOffset, end - m_layout.fileNameOffset);
}

std::span<const std::byte> DirectoryRecord::bytes() const
{
  return m_bytes.first(std::min<size_t>(size(), m_bytes.size()));
}

ULONG DirectoryRecord::readField(ULONG offset) const
{
  ULONG value = 0;
  if (offset + sizeof(value) <= m_bytes.size()) {
    std::memcpy(&value, m_bytes.data() + offset, sizeof(value));
  }
  return value;
}

namespace
{

// ntdll starts every record a multiple of 8 bytes from the start of the buffer, for
// all twelve classes alike, whatever the alignment of their records
constexpr size_t RECORD_SPACING = 8;

void writeField(std::span<std::byte> record, ULONG offset, ULONG value)
{
  std::memcpy(record.data() + offset, &value, sizeof(value));
}

bool isError(NTSTATUS status)
{
  return (static_cast<ULONG>(status) >> 30) == 3;
}

}  // namespace

DirectoryOutput::DirectoryOutput(std::span<std::byte> buffer,
                                 const DirectoryRecordLayout& layout, Options options)
    : m_buffer(buffer), m_layout(layout), m_options(options)
{}

bool DirectoryOutput::isEmpty() const
{
  return !m_lastRecordOffset.has_value();
}

bool DirectoryOutput::isSatisfied() const
{
  return m_options.isSingleEntry && !isEmpty();
}

bool DirectoryOutput::isOverflowed() const
{
  return m_isOverflowed;
}

ULONG DirectoryOutput::size() const
{
  return static_cast<ULONG>(m_size);
}

bool DirectoryOutput::append(const DirectoryRecord& record)
{
  if (record.isTruncated()) {
    return false;
  }
  const std::optional<std::span<std::byte>> target = place(record.size());
  if (!target) {
    return false;
  }
  std::ranges::copy(record.bytes().first(target->size()), target->begin());
  writeField(*target, 0, 0);
  return !m_isOverflowed;
}

bool DirectoryOutput::appendRenamed(const DirectoryRecord& record,
                                    std::wstring_view name)
{
  if (record.isTruncated()) {
    return false;
  }
  const ULONG fileNameLength = static_cast<ULONG>(name.size() * sizeof(WCHAR));
  const std::optional<std::span<std::byte>> target =
      place(m_layout.fileNameOffset + fileNameLength);
  if (!target) {
    return false;
  }
  std::ranges::copy(record.bytes().first(m_layout.fileNameOffset), target->begin());
  writeField(*target, 0, 0);
  writeField(*target, m_layout.fileNameLengthOffset, fileNameLength);
  std::memcpy(
      target->data() + m_layout.fileNameOffset, name.data(),
      std::min<size_t>(fileNameLength, target->size() - m_layout.fileNameOffset));

  // An 8.3 name is an alias that the filesystem made unique only in the directory of
  // the record, and GetShortPathNameW builds paths of the 8.3 names that a listing
  // returns, so it would build paths that lead nowhere or to another file. NTFS and
  // ReFS zero the fields of a record that has no 8.3 name.
  if (m_layout.shortName) {
    std::fill(target->begin() + m_layout.shortName->lengthOffset,
              target->begin() + m_layout.shortName->offset + 12 * sizeof(WCHAR),
              std::byte{0});
  }
  return !m_isOverflowed;
}

void DirectoryOutput::mirror(std::span<const std::byte> native)
{
  std::ranges::copy(native, m_buffer.begin());
  m_lastRecordOffset = 0;
  m_size             = native.size();
  m_isOverflowed     = true;
}

NTSTATUS DirectoryOutput::complete(NTSTATUS fillStatus,
                                   IO_STATUS_BLOCK& statusBlock) const
{
  NTSTATUS status = fillStatus;
  if (fillStatus == STATUS_NO_MORE_FILES) {
    // records written by this query are reported first, the end by the next one;
    // ending without any record is an error on the first query only
    status = !isEmpty()              ? STATUS_SUCCESS
             : m_options.isFirstCall ? STATUS_NO_SUCH_FILE
                                     : STATUS_NO_MORE_FILES;
  }
  if (!isError(status)) {
    statusBlock.Status      = status;
    statusBlock.Information = m_size;
  }
  return status;
}

std::optional<std::span<std::byte>> DirectoryOutput::place(size_t recordSize)
{
  if (isSatisfied()) {
    return std::nullopt;
  }
  // NtfsQueryDirectory and RefsQueryDirectory cut a first record that does not fit
  // short at the end of the buffer, and leave its entry for the next query to return
  // again. The buffer holds the fields before FileName at least, as ntdll rejects one
  // shorter than the minimum length.
  if (isEmpty() && recordSize > m_buffer.size()) {
    m_lastRecordOffset = 0;
    m_size             = m_buffer.size();
    m_isOverflowed     = true;
    return m_buffer;
  }
  const size_t offset =
      isEmpty() ? 0 : (m_size + RECORD_SPACING - 1) / RECORD_SPACING * RECORD_SPACING;
  if (offset + recordSize > m_buffer.size()) {
    return std::nullopt;
  }
  if (m_lastRecordOffset.has_value()) {
    writeField(m_buffer.subspan(*m_lastRecordOffset), 0,
               static_cast<ULONG>(offset - *m_lastRecordOffset));
  }
  m_lastRecordOffset = offset;
  m_size             = offset + recordSize;
  return m_buffer.subspan(offset, recordSize);
}
