// Tests auto-enregistrés : chaque tests/checks_<domaine>.c est lié dans
// abi-smoke par le Makefile et déclare ses fonctions avec RT_CHECK(nom).
// Elles s'exécutent après les tests de base, runtime déjà initialisé et
// région 0x20000000-0x20020000 projetée. Un échec doit appeler assert/abort.
#pragma once
#include "rt.h"
#include <assert.h>
typedef void (*rt_check_fn)(void);
void rt_register_check(const char *name, rt_check_fn fn);
#define RT_CHECK(fn) \
    static void fn(void); \
    __attribute__((constructor)) static void rt_check_register_##fn(void) { rt_register_check(#fn, fn); } \
    static void fn(void)
