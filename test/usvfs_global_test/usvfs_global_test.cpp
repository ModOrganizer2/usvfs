#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "../usvfs_global_test_runner/gtest_utils.h"

class
{
  bool initialize(int argc, char* argv[])
  {
    // extract the data path
    if (argc != 2) {
      std::cerr << "missing data path, aborting\n";
      return false;
    }
    m_data = std::filesystem::path{argv[1]};

    if (!exists(m_data)) {
      std::cerr << "data path '" << m_data.string() << "'does not exist\n";
      return false;
    }

    return true;
  }

  friend int main(int argc, char* argv[]);
  std::filesystem::path m_data;

public:
  const auto& data() const { return m_data; }
} parameters;

// simple function to write content to a specified path
void write_content(const std::filesystem::path& path, const std::string_view content)
{
  std::ofstream ofs{path};
  ofs << content;
}

TEST(BasicTest, SimpleTest)
{
  const auto data = parameters.data();

  ASSERT_TRUE(exists(data));
  ASSERT_TRUE(exists(data / "docs"));
  ASSERT_TRUE(exists(data / "empty"));
  ASSERT_TRUE(exists(data / "docs" / "doc.txt"));
  ASSERT_TRUE(exists(data / "readme.txt"));
}

TEST(RedFileSystemTest, RedFileSystemTest)
{
  const auto data = parameters.data();

  const auto storages_path   = data / "r6" / "storages";
  const auto hudpainter_path = storages_path / "HUDPainter";

  ASSERT_TRUE(exists(hudpainter_path / "DEFAULT.json"));

  ASSERT_EQ(hudpainter_path / "TEST.json",
            weakly_canonical(hudpainter_path / "TEST.json"));

  write_content(hudpainter_path / "TEST.json", "{}");
}

TEST(SkipFilesTest, SkipFilesTest)
{
  const auto data = parameters.data();

  // file in mod1 should have been skipped
  ASSERT_FALSE(exists(data / "readme.skip"));

  // docs/doc.skip should come from data, not mods1/docs/doc.skip
  ASSERT_CONTENT_EQ("doc.skip in data/docs", data / "docs" / "doc.skip");

  // write to readme.skip should create a file in overwrite
  write_content(data / "readme.skip", "readme.skip in overwrite");
}

int main(int argc, char* argv[])
{
  testing::InitGoogleTest(&argc, argv);

  // initialize parameters from remaining arguments
  if (!parameters.initialize(argc, argv)) {
    return -1;
  }

  return RUN_ALL_TESTS();
}