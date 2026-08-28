#pragma once

#include <stdio.h>

#define NTYPE_MAX_BUTTONS 26

typedef struct NTypeNode NTypeNode;

struct NTypeNode {
        char *symbol;
        unsigned char n_buttons;
        NTypeNode *click[NTYPE_MAX_BUTTONS];
        NTypeNode *hold[NTYPE_MAX_BUTTONS];
};

/* Split a comma-separated alphabet (high → low frequency) into a newly
 * allocated array of strings. Returns the count; *symbols is NULL on empty. */
int separate_symbols(const char *input, char ***symbols);
void free_symbols(char **symbols, int count);

/* n-ary Huffman prefix tree. n_buttons is 1..26 (a byte, English letters).
 * Slot order: a,b,... clicks, then A,B,... holdes. Leaves hold symbols. */
int build_ntype_tree(NTypeNode **root, const char **symbols, int count,
                     int n_buttons);
void free_ntype_tree(NTypeNode *root);

void print_ntype_codes(const NTypeNode *root);
int export_ntype_json(const NTypeNode *root, FILE *fp);
