#pragma once

#include <adwaita.h>

#include "paint-preferences.h"
#include "paint-tools.h"

G_BEGIN_DECLS

#define PAINT_APP_ID "io.github.spondle.Paint"
#define PAINT_APP_NAME "Paint"
#define PAINT_APP_VERSION "0.4.0"

#define PAINT_TYPE_APPLICATION (paint_application_get_type ())
G_DECLARE_FINAL_TYPE (PaintApplication, paint_application, PAINT, APPLICATION, AdwApplication)

PaintApplication *paint_application_new (void);
PaintPreferences *paint_application_get_preferences (PaintApplication *self);
PaintToolSettings *paint_application_get_tool_settings (PaintApplication *self);

G_END_DECLS
