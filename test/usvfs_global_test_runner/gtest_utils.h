#pragma once

// this file is shared by both usvfs_global_test and usvfs_global_test_runner
//

#include <filesystem>

#include <gtest/gtest.h>

::testing::AssertionResult AssertDirectoryEquals(const std::filesystem::path& expected,
                                                 const std::filesystem::path& actual,
                                                 bool content = true);

::testing::AssertionResult AssertContentEquals(std::string_view expected,
                                               const std::filesystem::path& path,
                                               bool trim = true);

#define ASSERT_DIRECTORY_EQ(Expected, Actual)                                          \
  ASSERT_TRUE(AssertDirectoryEquals(Expected, Actual))

#define ASSERT_CONTENT_EQ(Expected, Path)                                              \
  ASSERT_TRUE(AssertContentEquals(Expected, Path))