#include "paint-application.h"

#include <locale.h>

#include "paint-i18n.h"

int
main (int argc, char **argv)
{
#ifdef LOCALEDIR
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
#endif
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);
  setlocale (LC_ALL, "");

  g_autoptr (PaintApplication) app = paint_application_new ();
  return g_application_run (G_APPLICATION (app), argc, argv);
}
