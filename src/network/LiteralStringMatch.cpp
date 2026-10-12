#include "network/LiteralStringMatch.h"

namespace FastBee {
#if defined(__GNUC__) && !defined(__clang__)
#define FASTBEE_LITERAL_STRING_MATCH __attribute__((noinline, noclone))
#elif defined(__clang__)
#define FASTBEE_LITERAL_STRING_MATCH __attribute__((noinline))
#else
#define FASTBEE_LITERAL_STRING_MATCH
#endif

FASTBEE_LITERAL_STRING_MATCH
bool stringEqualsIgnoreCaseLiteral(const String& value, const char* literal) {
    return value.equalsIgnoreCase(String(literal));
}

#undef FASTBEE_LITERAL_STRING_MATCH
}
