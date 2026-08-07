#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int escape_enabled = 0;

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void print_escaped(const char *s) {
    while (*s) {
        if (*s == '\\' && escape_enabled) {
            s++;
            switch (*s) {
                case 'a':  putchar('\a'); break;
                case 'b':  putchar('\b'); break;
                case 'e':  putchar(0x1B); break;
                case 'f':  putchar('\f'); break;
                case 'n':  putchar('\n'); break;
                case 'r':  putchar('\r'); break;
                case 't':  putchar('\t'); break;
                case 'v':  putchar('\v'); break;
                case '\\': putchar('\\'); break;
                case '0': {
                    int val = 0;
                    int digits = 0;
                    s++;
                    while (digits < 3 && *s >= '0' && *s <= '7') {
                        val = val * 8 + (*s - '0');
                        s++;
                        digits++;
                    }
                    if (digits > 0) putchar(val);
                    s--;
                    break;
                }
                case 'x': {
                    s++;
                    int hi = hex_digit(*s);
                    if (hi >= 0) {
                        s++;
                        int lo = hex_digit(*s);
                        if (lo >= 0) {
                            putchar((hi << 4) | lo);
                        } else {
                            putchar('\\');
                            putchar('x');
                            s -= 2;
                        }
                    } else {
                        putchar('\\');
                        s--;
                    }
                    break;
                }
                case '\0':
                    putchar('\\');
                    return;
                default:
                    putchar('\\');
                    putchar(*s);
                    break;
            }
        } else {
            putchar(*s);
        }
        s++;
    }
}

int main(int argc, char *argv[]) {
    int no_newline = 0;
    int first_arg = 1;

    while (first_arg < argc && argv[first_arg][0] == '-') {
        if (strcmp(argv[first_arg], "--") == 0) {
            first_arg++;
            break;
        }
        if (strcmp(argv[first_arg], "-n") == 0) {
            no_newline = 1;
            first_arg++;
            continue;
        }
        if (strcmp(argv[first_arg], "-e") == 0) {
            escape_enabled = 1;
            first_arg++;
            continue;
        }
        if (strcmp(argv[first_arg], "-E") == 0) {
            escape_enabled = 0;
            first_arg++;
            continue;
        }
        break;
    }

    for (int i = first_arg; i < argc; i++) {
        if (i > first_arg) putchar(' ');
        print_escaped(argv[i]);
    }

    if (!no_newline) putchar('\n');

    return 0;
}
