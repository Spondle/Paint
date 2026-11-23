#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

void paint_window_save_all_sessions (AdwApplication *app);
gboolean paint_window_restore_session (AdwApplication *app);
void paint_window_clear_session (void);

void paint_session_schedule_autosave (AdwApplication *app);
void paint_session_cancel_autosave (void);

G_END_DECLS
