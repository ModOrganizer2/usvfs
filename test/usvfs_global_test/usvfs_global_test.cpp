#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

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

  {
    std::ofstream of{hudpainter_path / "TEST.json"};
    of << "{}\n";
  }

  // ASSERT_EQ(std::filesystem::path{}, weakly_canonical(hudpainter_path /
  // "TEST.json"));
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