#include <stdio.h>
#include <stdlib.h>

void __assert_fail(const char *assertion, const char *file, unsigned int line,
                   const char *function) {
    fprintf(stderr, "%s:%u: %s%sAssertion `%s' failed.\n",
            file, line,
            function ? function : "",
            function ? ": " : "",
            assertion);
    abort();
}
