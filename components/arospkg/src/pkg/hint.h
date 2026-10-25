#ifndef APKG_HINT_H
#define APKG_HINT_H
#include <stddef.h>
/* A command a user can copy: "apkg <args>", with a custom --root passed on
   and quoted for the Shell. */
void cmd_hint(char *out, size_t n, const char *root, const char *args);
void open_hint(char *out, size_t n, const char *root, const char *id);
#endif
