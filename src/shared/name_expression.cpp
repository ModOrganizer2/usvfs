#include "name_expression.h"

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

// the DOS wildcards, under the names ntifs.h gives them
constexpr WCHAR DOS_STAR = L'<';
constexpr WCHAR DOS_QM   = L'>';
constexpr WCHAR DOS_DOT  = L'"';

// A character of the name, folded as the comparison asks, with what the DOS wildcards
// need to know of where it lies.
struct NameCharacter
{
  WCHAR value;
  // whether it is the dot that no other one follows
  bool isFinalDot;
  bool isLast;
};

// Returns whether an element of the expression may match no characters before the
// next character of the name, or before its end for nothing.
bool canMatchNothing(WCHAR element, const std::optional<NameCharacter>& next)
{
  switch (element) {
  case L'*':
  case DOS_STAR:
    return true;
  // a run of DOS_QM ends early at a dot or at the end of the name
  case DOS_QM:
    return !next || next->value == L'.';
  // matches a dot, or nothing beyond the end of the name
  case DOS_DOT:
    return !next;
  default:
    return false;
  }
}

// where reading a character of the name takes the expression from an element
enum class Move
{
  Mismatch,
  Stay,
  Advance,
};

// Returns where reading a character of the name takes the expression from an element.
Move readCharacter(WCHAR element, const NameCharacter& c)
{
  switch (element) {
  case L'*':
    return Move::Stay;
  // any character up to the final dot, which ends its run
  case DOS_STAR:
    return c.isFinalDot ? Move::Advance : Move::Stay;
  case L'?':
    return Move::Advance;
  // any character but a dot, unless the dot ends the name
  case DOS_QM:
    return c.value != L'.' || c.isLast ? Move::Advance : Move::Mismatch;
  // a dot, or a literal quote, as ntdll matches one
  case DOS_DOT:
    return c.value == L'.' || c.value == DOS_DOT ? Move::Advance : Move::Mismatch;
  default:
    return element == c.value ? Move::Advance : Move::Mismatch;
  }
}

// Runs the expression as a nondeterministic automaton whose states are its positions,
// the one past its last element included, over the characters of a name: call
// skipEmptyMatches before reading each character and once after the last, then
// isMatched. That takes time proportional to the product of both lengths, where
// backtracking over the stars can take exponential time.
class ExpressionAutomaton
{
public:
  explicit ExpressionAutomaton(std::wstring_view expression)
      : m_expression(expression), m_isReached(expression.size() + 1),
        m_isReachedNext(expression.size() + 1)
  {
    m_isReached.front() = true;
  }

  // Moves on past the elements that match nothing before the next character, or
  // before the end of the name for nothing. One pass in the order of the positions
  // follows a run of such elements, as matching nothing only moves forward.
  void skipEmptyMatches(const std::optional<NameCharacter>& next)
  {
    for (size_t position = 0; position < m_expression.size(); ++position) {
      if (m_isReached[position] && canMatchNothing(m_expression[position], next)) {
        m_isReached[position + 1] = true;
      }
    }
  }

  // Reads a character of the name. Returns whether the expression still reaches some
  // position; once it reaches none, no further character can make it match.
  bool read(const NameCharacter& c)
  {
    std::fill(m_isReachedNext.begin(), m_isReachedNext.end(), false);
    bool isAnyReached = false;
    for (size_t position = 0; position < m_expression.size(); ++position) {
      if (!m_isReached[position]) {
        continue;
      }
      switch (readCharacter(m_expression[position], c)) {
      case Move::Mismatch:
        continue;
      case Move::Stay:
        m_isReachedNext[position] = true;
        break;
      case Move::Advance:
        m_isReachedNext[position + 1] = true;
        break;
      }
      isAnyReached = true;
    }
    std::swap(m_isReached, m_isReachedNext);
    return isAnyReached;
  }

  // whether the characters read match the whole expression
  bool isMatched() const { return m_isReached.back(); }

private:
  std::wstring_view m_expression;
  // whether the characters read so far can bring the expression to each position
  std::vector<bool> m_isReached;
  // where read gathers the positions reached, kept to spare an allocation per character
  std::vector<bool> m_isReachedNext;
};

}  // namespace

BOOLEAN NTAPI usvfs::shared::isNameInExpression(PUNICODE_STRING Expression,
                                                PUNICODE_STRING Name,
                                                BOOLEAN IgnoreCase, PWCH UpcaseTable)
{
  const std::wstring_view expression(Expression->Buffer,
                                     Expression->Length / sizeof(WCHAR));
  const std::wstring_view name(Name->Buffer, Name->Length / sizeof(WCHAR));
  // as ntdll answers, though a star matches nothing at the end of any other name
  if (name.empty()) {
    return expression.empty();
  }

  const auto fold = [&](WCHAR c) {
    if (!IgnoreCase) {
      return c;
    }
    return UpcaseTable != nullptr ? UpcaseTable[c] : RtlUpcaseUnicodeChar(c);
  };
  const size_t finalDot = name.rfind(L'.');
  ExpressionAutomaton automaton(expression);
  for (size_t i = 0; i < name.size(); ++i) {
    const NameCharacter c{fold(name[i]), i == finalDot, i + 1 == name.size()};
    automaton.skipEmptyMatches(c);
    if (!automaton.read(c)) {
      return FALSE;
    }
  }
  automaton.skipEmptyMatches(std::nullopt);
  return automaton.isMatched();
}
