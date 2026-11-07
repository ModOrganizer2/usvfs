#include <gtest/gtest.h>
#include <name_expression.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace
{

UNICODE_STRING toUnicodeString(std::wstring_view text)
{
  const auto length = static_cast<USHORT>(text.size() * sizeof(WCHAR));
  // only read, though UNICODE_STRING has no const buffer
  return {length, length, const_cast<PWSTR>(text.data())};
}

bool matches(std::wstring_view expression, std::wstring_view name,
             bool ignoreCase = true, PWCH upcaseTable = nullptr)
{
  UNICODE_STRING expressionString = toUnicodeString(expression);
  UNICODE_STRING nameString       = toUnicodeString(name);
  return usvfs::shared::isNameInExpression(&expressionString, &nameString, ignoreCase,
                                           upcaseTable) != FALSE;
}

// ntdll's own, which the emulation is compared with
RtlIsNameInExpression_type nativeIsNameInExpression()
{
  return reinterpret_cast<RtlIsNameInExpression_type>(
      ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "RtlIsNameInExpression"));
}

// Returns every string of up to maxLength characters of the alphabet, the empty one
// included.
std::vector<std::wstring> allStrings(std::wstring_view alphabet, size_t maxLength)
{
  std::vector<std::wstring> strings{L""};
  size_t lengthStart = 0;
  for (size_t length = 1; length <= maxLength; ++length) {
    const size_t lengthEnd = strings.size();
    for (size_t i = lengthStart; i < lengthEnd; ++i) {
      for (const WCHAR c : alphabet) {
        strings.push_back(strings[i] + c);
      }
    }
    lengthStart = lengthEnd;
  }
  return strings;
}

// Expects the emulation to answer as ntdll does for every pair of an expression and a
// name of the alphabets.
void expectSameAsNtdll(const std::vector<std::wstring>& expressions,
                       const std::vector<std::wstring>& names, bool ignoreCase,
                       PWCH upcaseTable = nullptr)
{
  const RtlIsNameInExpression_type native = nativeIsNameInExpression();
  ASSERT_NE(native, nullptr);
  size_t mismatches = 0;
  for (const std::wstring& expression : expressions) {
    for (const std::wstring& name : names) {
      UNICODE_STRING expressionString = toUnicodeString(expression);
      UNICODE_STRING nameString       = toUnicodeString(name);
      const bool expected =
          native(&expressionString, &nameString, ignoreCase, upcaseTable) != FALSE;
      if (matches(expression, name, ignoreCase, upcaseTable) != expected &&
          ++mismatches <= 20) {
        ADD_FAILURE() << "expression " << testing::PrintToString(expression)
                      << ", name " << testing::PrintToString(name) << ": ntdll answers "
                      << expected;
      }
    }
  }
  EXPECT_EQ(mismatches, 0u);
}

struct NameExpressionCase
{
  std::wstring_view expression;
  std::wstring_view name;
  bool isMatch;
};

}  // namespace

// What ntdll answers, which documents what each wildcard matches; the comparisons
// below check every case of their alphabets.
TEST(NameExpressionTest, MatchesAsNtdllDoes)
{
  constexpr std::array cases{
      // an empty name matches only an empty expression, not even a star
      NameExpressionCase{L"", L"", true},
      NameExpressionCase{L"*", L"", false},
      NameExpressionCase{L"", L"A", false},
      // * matches any characters, ? any one
      NameExpressionCase{L"*", L"a.b", true},
      NameExpressionCase{L"A*B", L"ab", true},
      NameExpressionCase{L"?", L".", true},
      NameExpressionCase{L"A?", L"a", false},
      // < matches up to the final dot, which ends its run
      NameExpressionCase{L"<.C", L"a.b.c", true},
      NameExpressionCase{L"<", L"a.b", false},
      NameExpressionCase{L"<", L"a.", true},
      NameExpressionCase{L"<B", L"a.b", true},
      // > matches any character but a dot, and nothing at a dot or the end of the
      // name, which ends its run; a dot that ends the name it matches too
      NameExpressionCase{L">>", L"a", true},
      NameExpressionCase{L">>.B", L"a.b", true},
      NameExpressionCase{L">", L".a", false},
      NameExpressionCase{L">", L".", true},
      NameExpressionCase{L"A>", L"a.", true},
      NameExpressionCase{L"A>", L"a..", false},
      // " matches a dot, or nothing beyond the end of the name
      NameExpressionCase{L"A\"B", L"a.b", true},
      NameExpressionCase{L"A\"B", L"ab", false},
      NameExpressionCase{L"A\"", L"a", true},
      NameExpressionCase{L"A\"", L"a\"", true},
      // with IgnoreCase, the name is upcased, the expression is not
      NameExpressionCase{L"\u00C4", L"\u00E4", true},
      NameExpressionCase{L"a", L"a", false},
  };
  for (const NameExpressionCase& c : cases) {
    EXPECT_EQ(matches(c.expression, c.name), c.isMatch)
        << "expression " << testing::PrintToString(std::wstring(c.expression))
        << ", name " << testing::PrintToString(std::wstring(c.name));
  }
}

TEST(NameExpressionTest, AnswersAsNtdllForEveryShortWildcardExpression)
{
  expectSameAsNtdll(allStrings(L"AB.*?<>\"", 4), allStrings(L"aB.", 5), true);
}

TEST(NameExpressionTest, AnswersAsNtdllForWildcardsInNames)
{
  expectSameAsNtdll(allStrings(L"A\u00C4.*?<>\"", 3), allStrings(L"a\u00E4.*?<>\"", 4),
                    true);
}

TEST(NameExpressionTest, AnswersAsNtdllWithoutIgnoringCase)
{
  expectSameAsNtdll(allStrings(L"aA.*<>", 3), allStrings(L"aA.", 4), false);
}

TEST(NameExpressionTest, AnswersAsNtdllWithAnUpcaseTable)
{
  // folds only 'x', to 'A', so that the table rather than the default upcasing decides
  std::vector<WCHAR> upcaseTable(0x10000);
  for (size_t c = 0; c < upcaseTable.size(); ++c) {
    upcaseTable[c] = static_cast<WCHAR>(c);
  }
  upcaseTable[L'x'] = L'A';
  expectSameAsNtdll(allStrings(L"Aax*", 3), allStrings(L"aAx.", 3), true,
                    upcaseTable.data());
}
