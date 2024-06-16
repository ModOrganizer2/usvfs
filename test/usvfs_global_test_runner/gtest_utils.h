#pragma once

#include <filesystem>
#include <format>

#include <boost/algorithm/string.hpp>

#include <gtest/gtest.h>

::testing::AssertionResult AssertDirectoryEquals(const std::filesystem::path& expected,
                                                 const std::filesystem::path& actual,
                                                 bool content = true)
{
  std::vector<std::string> failure_messages;
  std::vector<std::filesystem::path> in_both;

  // iterate left, check on right
  for (const auto& it : std::filesystem::recursive_directory_iterator{expected}) {
    const auto relpath = relative(it.path(), expected);
    if (!exists(actual / relpath)) {
      failure_messages.push_back(
          std::format("{} expected but not found", relpath.string()));
    } else {
      in_both.push_back(relpath);
    }
  }

  // iterate right, check on left
  for (const auto& it : std::filesystem::recursive_directory_iterator{actual}) {
    const auto relpath = relative(it.path(), actual);
    if (!exists(expected / relpath)) {
      failure_messages.push_back(
          std::format("{} found but not expected", relpath.string()));
    }
  }

  // check contents
  if (content) {
    for (const auto& relpath : in_both) {
      const auto expected_path = expected / relpath, actual_path = actual / relpath;

      if (is_directory(expected_path) != is_directory(actual_path)) {
        failure_messages.push_back(
            std::format("{} type mismatch, expected {} but found {}", relpath.string(),
                        is_directory(expected_path) ? "directory" : "file",
                        is_directory(expected_path) ? "file" : "directory"));
        continue;
      }

      if (is_directory(expected_path)) {
        continue;
      }

      if (!test::compare_files(expected_path, actual_path, true)) {
        failure_messages.push_back(
            std::format("{} content mismatch", relpath.string()));
      }
    }
  }

  if (failure_messages.empty()) {
    return ::testing::AssertionSuccess();
  }

  return ::testing::AssertionFailure()
         << "\n"
         << boost::algorithm::join(failure_messages, "\n") << "\n";
}

#define ASSERT_DIRECTORY_EQ(Expected, Actual)                                          \
  ASSERT_TRUE(AssertDirectoryEquals(Expected, Actual))