#include "utils.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0) {
    printf("Usage: %s gen [-b N] <alphabet> [export.json]\n", argv0);
    printf("\n");
    printf("  alphabet     comma-separated symbols, highest frequency first\n");
    printf(
        "  -b N         button count (default 1, max %d). Stored as a byte.\n",
        NTYPE_MAX_BUTTONS);
    printf("  export.json  optional path; write the tree for other projects\n");
    printf("\n");
    printf("  Buttons are named a, b, ... up to N English letters.\n");
    printf("  lowercase = click, uppercase = hold (e.g. a, A, b, B).\n");
    printf("  A leaf is a finished symbol (prefix-free).\n");
}

static int parse_buttons(const char *s, int *n_buttons) {
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < 1 || v > NTYPE_MAX_BUTTONS) {
        fprintf(stderr,
                "Button count must be 1..%d (one byte, English letters)\n",
                NTYPE_MAX_BUTTONS);
        return -1;
    }
    *n_buttons = (int)v;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "-h") == 0 ||
        strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "help") == 0) {
        usage(argv[0]);
        return argc < 2 ? 1 : 0;
    }
    if (strcmp(argv[1], "gen") != 0) {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        usage(argv[0]);
        return 1;
    }

    int n_buttons = 1;
    const char *alphabet = NULL;
    const char *export_path = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--buttons") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0]);
                return 1;
            }
            if (parse_buttons(argv[++i], &n_buttons) != 0) {
                return 1;
            }
        } else if (!alphabet) {
            alphabet = argv[i];
        } else if (!export_path) {
            export_path = argv[i];
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (!alphabet) {
        usage(argv[0]);
        return 1;
    }

    char **symbols = NULL;
    int count = separate_symbols(alphabet, &symbols);
    if (count <= 0) {
        fprintf(stderr, "No symbols in alphabet\n");
        free_symbols(symbols, count);
        return 1;
    }

    NTypeNode *root = NULL;
    if (build_ntype_tree(&root, (const char **)symbols, count, n_buttons) !=
        0) {
        fprintf(stderr, "Failed to build tree\n");
        free_symbols(symbols, count);
        return 1;
    }

    print_ntype_codes(root);

    if (export_path) {
        FILE *fp = fopen(export_path, "w");
        if (!fp) {
            perror(export_path);
            free_ntype_tree(root);
            free_symbols(symbols, count);
            return 1;
        }
        if (export_ntype_json(root, fp) != 0) {
            fprintf(stderr, "Failed to export %s\n", export_path);
            fclose(fp);
            free_ntype_tree(root);
            free_symbols(symbols, count);
            return 1;
        }
        fclose(fp);
        printf("exported %s\n", export_path);
    }

    free_ntype_tree(root);
    free_symbols(symbols, count);
    return 0;
}
