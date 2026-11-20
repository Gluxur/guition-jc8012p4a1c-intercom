#include "test_menu.h"

#include "unity.h"        // UNITY_BEGIN/UNITY_END, test_desc_t, unity_run_*
#include "unity_config.h" // unity_gets

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tst {
namespace {

void trim_trailing_space(char *s) {
  char *end = s + strlen(s) - 1;
  while (end >= s && isspace(static_cast<unsigned char>(*end))) {
    *end = 0;
    --end;
  }
}

int print_menu() {
  const int count = unity_get_test_count();
  printf("\n\nHere's the test menu, pick your combo:\n");
  for (int i = 0; i < count; ++i) {
    test_desc_t d = {};
    if (unity_get_test_info(i, &d)) {
      printf("(%d)\t\"%s\" %s\n", i + 1, d.name, d.desc);
    }
  }
  printf("(0)\tTOUT LANCER  (equivalent de '*')\n");
  printf("\t'[jpeg]' / '[bsp]' / '[audio]' : par tag  |  '!' prefixe : inverse\n");
  printf("\t'3 7 12' : plusieurs tests  |  '-' : rappel du dernier bilan\n");
  // unit_test.py cherche cette phrase exacte pour detecter la fin du menu :
  // ne pas la reformuler.
  printf("\nEnter test for running.\n");
  fflush(stdout);
  return count;
}

/* « 3 7 12 » ou « 3,7,12 » : chaque entier est un numero de test (base 1,
 * comme affiche). unity_run_test_by_index, lui, est en base 0. */
void run_indexes(const char *s, int count) {
  const char *p = s;
  while (*p != '\0') {
    while (*p != '\0' && isdigit(static_cast<unsigned char>(*p)) == 0) {
      ++p;
    }
    if (*p == '\0') {
      break;
    }
    char *next = nullptr;
    const long idx = strtol(p, &next, 10);
    p = next;
    if (idx >= 1 && idx <= count) {
      unity_run_test_by_index(static_cast<int>(idx) - 1);
    } else {
      printf("index %ld hors bornes (1-%d)\n", idx, count);
    }
  }
}

} // namespace

void run_menu() {
  printf("\n\nAppuyer sur ENTREE pour afficher la liste des tests.\n");
  fflush(stdout);

  int count = unity_get_test_count();
  for (;;) {
    char cmdline[256] = {};
    while (strlen(cmdline) == 0) {
      unity_gets(cmdline, sizeof(cmdline));
      trim_trailing_space(cmdline);
      if (strlen(cmdline) == 0) {
        count = print_menu();
      }
    }

    // '-' rappelle le bilan precedent : doit passer AVANT UNITY_BEGIN, qui
    // remet l'historique a zero.
    if (cmdline[0] == '-') {
      UNITY_END();
      continue;
    }

    UNITY_BEGIN();

    size_t idx = 0;
    bool invert = false;
    if (cmdline[idx] == '!') {
      invert = true;
      ++idx;
    }

    if (cmdline[idx] == '*' || (cmdline[idx] == '0' && cmdline[idx + 1] == '\0')) {
      unity_run_all_tests();
    } else if (cmdline[idx] == '[') {
      unity_run_tests_by_tag(cmdline + idx, invert);
    } else if (cmdline[idx] == '"') {
      char *end = strrchr(cmdline, '"');
      if (end > &cmdline[idx]) {
        *end = 0;
        unity_run_test_by_name(cmdline + idx + 1);
      }
    } else if (isdigit(static_cast<unsigned char>(cmdline[idx])) != 0) {
      run_indexes(cmdline + idx, count);
    } else {
      printf("Entree non reconnue : %s\n", cmdline);
    }

    UNITY_END();

    printf("Test suivant, ou 'entree' pour le menu\n");
    fflush(stdout);
  }
}

} // namespace tst
