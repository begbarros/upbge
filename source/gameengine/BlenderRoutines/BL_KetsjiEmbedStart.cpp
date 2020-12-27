/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 * Blender's Ketsji startpoint
 */

/** \file gameengine/BlenderRoutines/BL_KetsjiEmbedStart.cpp
 *  \ingroup blroutines
 */

#ifdef _MSC_VER
/* don't show stl-warnings */
#  pragma warning(disable : 4786)
#endif

#include "BKE_context.h"
#include "BKE_global.h"
#include "BKE_main.h"
#include "BKE_report.h"
#include "BKE_undo_system.h"
#include "BLI_blenlib.h"
#include "BLO_readfile.h"
#include "DNA_space_types.h"
#include "ED_screen.h"
#include "WM_api.h"
#include "wm_window.h"

#include "CM_Message.h"
#include "GHOST_ISystem.h"
#include "KX_Globals.h"
#include "LA_BlenderLauncher.h"

extern "C" {

void StartKetsjiShell(struct bContext *C,
                      struct ARegion *ar,
                      rcti *cam_frame,
                      int always_use_expand_framing);
}

#ifdef WITH_AUDASPACE
#  include <AUD_Device.h>
#endif

static BlendFileData *load_game_data(const char *filename)
{
  ReportList reports;
  BlendFileData *bfd;

  BKE_reports_init(&reports, RPT_STORE);
  bfd = BLO_read_from_file(filename, BLO_READ_SKIP_USERDEF, &reports);

  if (!bfd) {
    CM_Error("loading " << filename << " failed: ");
    BKE_reports_print(&reports, RPT_ERROR);
  }

  BKE_reports_clear(&reports);

  return bfd;
}

static void InitBlenderContextVariables(bContext *C, wmWindowManager *wm, Scene *scene)
{
  ARegion *ar;
  wmWindow *win;
  for (win = (wmWindow *)wm->windows.first; win; win = win->next) {
    bScreen *screen = WM_window_get_active_screen(win);
    if (!screen) {
      continue;
    }

    for (ScrArea *sa = (ScrArea *)screen->areabase.first; sa; sa = sa->next) {
      if (sa->spacetype == SPACE_VIEW3D) {
        ListBase *regionbase = &sa->regionbase;
        for (ar = (ARegion *)regionbase->first; ar; ar = ar->next) {
          if (ar->regiontype == RGN_TYPE_WINDOW) {
            if (ar->regiondata) {
              CTX_wm_screen_set(C, screen);
              CTX_wm_area_set(C, sa);
              CTX_wm_region_set(C, ar);
              CTX_data_scene_set(C, scene);
              win->scene = scene;
              return;
            }
          }
        }
      }
    }
  }
}

extern "C" void StartKetsjiShell(struct bContext *C,
                                 struct ARegion *ar,
                                 rcti *cam_frame,
                                 int always_use_expand_framing)
{
  /* context values */
  Scene *startscene = CTX_data_scene(C);
  Main *maggie1 = CTX_data_main(C);

  KX_ExitRequest exitrequested = KX_ExitRequest::NO_REQUEST;
  Main *blenderdata = maggie1;

  char *startscenename = startscene->id.name + 2;
  char pathname[FILE_MAXDIR + FILE_MAXFILE];
  char prevPathName[FILE_MAXDIR + FILE_MAXFILE];
  std::string exitstring = "";
  BlendFileData *bfd = nullptr;

  BLI_strncpy(pathname, blenderdata->name, sizeof(pathname));
  BLI_strncpy(prevPathName, G_MAIN->name, sizeof(prevPathName));

  /* Without this step, the bmain->name can be ".blend~"
   * and as I don't understand why and as the bug has been
   * reported, we ensure the extension is ".blend"
   * else this is causing issues with globalDict. (youle)
   */
  char *blend_name = blenderdata->name;
  BLI_path_extension_ensure(blend_name, FILE_MAX, ".blend");

  KX_SetOrigPath(std::string(blend_name));

#ifdef WITH_PYTHON

  // Acquire Python's GIL (global interpreter lock)
  // so we can safely run Python code and API calls
  PyGILState_STATE gilstate = PyGILState_Ensure();

  PyObject *globalDict = PyDict_New();

#endif

  GlobalSettings gs;
  gs.glslflag = startscene->gm.flag;

  if (startscene->gm.flag & GAME_USE_UNDO) {
    BKE_undosys_step_push(CTX_wm_manager(C)->undo_stack, C, "bge_start");
  }


  wmWindowManager *wm_backup = CTX_wm_manager(C);
  wmWindow *win_backup = CTX_wm_window(C);
  void *msgbus_backup = wm_backup->message_bus;
  void *gpuctx_backup = win_backup->gpuctx;
  void *ghostwin_backup = win_backup->ghostwin;

  do {
    // if we got an exitcode 3 (KX_ExitRequest::START_OTHER_GAME) load a different file
    if (exitrequested == KX_ExitRequest::START_OTHER_GAME ||
        exitrequested == KX_ExitRequest::RESTART_GAME) {
      exitrequested = KX_ExitRequest::NO_REQUEST;
      if (bfd) {
        /* Hack to not free the win->ghosting AND win->gpu_ctx when we restart/load new
         * .blend */
        CTX_wm_window(C)->ghostwin = nullptr;
        /* Hack to not free wm->message_bus when we restart/load new .blend */
        CTX_wm_manager(C)->message_bus = nullptr;
        BLO_blendfiledata_free(bfd);
      }

      char basedpath[FILE_MAX];
      // base the actuator filename with respect
      // to the original file working directory

      if (!exitstring.empty()) {
        BLI_strncpy(basedpath, exitstring.c_str(), sizeof(basedpath));
      }

      // load relative to the last loaded file, this used to be relative
      // to the first file but that makes no sense, relative paths in
      // blend files should be relative to that file, not some other file
      // that happened to be loaded first
      BLI_path_abs(basedpath, pathname);
      bfd = load_game_data(basedpath);

      // if it wasn't loaded, try it forced relative
      if (!bfd) {
        // just add "//" in front of it
        char temppath[FILE_MAX] = "//";
        BLI_strncpy(temppath + 2, basedpath, FILE_MAX - 2);

        BLI_path_abs(temppath, pathname);
        bfd = load_game_data(temppath);
      }

      // if we got a loaded blendfile, proceed
      if (bfd) {
        blenderdata = bfd->main;
        startscenename = bfd->curscene->id.name + 2;

        /* If we don't change G_MAIN, bpy won't work in loaded .blends */
        G_MAIN = G.main = bfd->main;
        CTX_data_main_set(C, bfd->main);
        wmWindowManager *wm = (wmWindowManager *)bfd->main->wm.first;
        wmWindow *win = (wmWindow *)wm->windows.first;
        CTX_wm_manager_set(C, wm);
        CTX_wm_window_set(C, win);
        win->ghostwin = ghostwin_backup;
        win->gpuctx = gpuctx_backup;
        wm->message_bus = (wmMsgBus *)msgbus_backup;
        /* We need to init Blender bContext environment here
         * to because in embedded, ar, v3d...
         * are needed for launcher creation
         */
        InitBlenderContextVariables(C, wm, bfd->curscene);
        wm_window_ghostwindow_embedded_ensure(wm, win);
        WM_check(C);

        if (blenderdata) {
          BLI_strncpy(pathname, blenderdata->name, sizeof(pathname));
          // Change G_MAIN path to ensure loading of data using relative paths.
          BLI_strncpy(G_MAIN->name, pathname, sizeof(G_MAIN->name));
        }
      }
      // else forget it, we can't find it
      else {
        exitrequested = KX_ExitRequest::QUIT_GAME;
      }
    }

    Scene *scene = bfd ? bfd->curscene :
                         (Scene *)BLI_findstring(
                             &blenderdata->scenes, startscenename, offsetof(ID, name) + 2);

    RAS_Rasterizer::StereoMode stereoMode = RAS_Rasterizer::RAS_STEREO_NOSTEREO;
    if (scene) {
      // Quad buffered needs a special window.
      if (scene->gm.stereoflag == STEREO_ENABLED) {
        if (scene->gm.stereomode != RAS_Rasterizer::RAS_STEREO_QUADBUFFERED) {
          switch (scene->gm.stereomode) {
            case STEREO_QUADBUFFERED: {
              stereoMode = RAS_Rasterizer::RAS_STEREO_QUADBUFFERED;
              break;
            }
            case STEREO_ABOVEBELOW: {
              stereoMode = RAS_Rasterizer::RAS_STEREO_ABOVEBELOW;
              break;
            }
            case STEREO_INTERLACED: {
              stereoMode = RAS_Rasterizer::RAS_STEREO_INTERLACED;
              break;
            }
            case STEREO_ANAGLYPH: {
              stereoMode = RAS_Rasterizer::RAS_STEREO_ANAGLYPH;
              break;
            }
            case STEREO_SIDEBYSIDE: {
              stereoMode = RAS_Rasterizer::RAS_STEREO_SIDEBYSIDE;
              break;
            }
            case STEREO_VINTERLACE: {
              stereoMode = RAS_Rasterizer::RAS_STEREO_VINTERLACE;
              break;
            }
            case STEREO_3DTVTOPBOTTOM: {
              stereoMode = RAS_Rasterizer::RAS_STEREO_3DTVTOPBOTTOM;
              break;
            }
          }
        }
      }
    }

    GHOST_ISystem *system = GHOST_ISystem::getSystem();
    LA_BlenderLauncher launcher = LA_BlenderLauncher(system,
                                                     blenderdata,
                                                     scene,
                                                     &gs,
                                                     stereoMode,
                                                     0,
                                                     nullptr,
                                                     C,
                                                     cam_frame,
                                                     CTX_wm_region(C),
                                                     always_use_expand_framing);
#ifdef WITH_PYTHON
    launcher.SetPythonGlobalDict(globalDict);
#endif  // WITH_PYTHON

    launcher.InitEngine();

    CM_Message(std::endl << "Blender Game Engine Started");
    launcher.EngineMainLoop();
    CM_Message("Blender Game Engine Finished");

    exitrequested = launcher.GetExitRequested();
    exitstring = launcher.GetExitString();
    gs = *launcher.GetGlobalSettings();

    launcher.ExitEngine();

    /* refer to WM_exit_ext() and BKE_blender_free(),
     * these are not called in the player but we need to match some of there behavior here,
     * if the order of function calls or blenders state isn't matching that of blender
     * proper, we may get troubles later on */
    WM_jobs_kill_all(CTX_wm_manager(C));

    for (wmWindow *win = (wmWindow *)CTX_wm_manager(C)->windows.first; win; win = win->next) {

      CTX_wm_window_set(C, win); /* needed by operator close callbacks */
      WM_event_remove_handlers(C, &win->handlers);
      WM_event_remove_handlers(C, &win->modalhandlers);
      ED_screen_exit(C, win, WM_window_get_active_screen(win));
    }
    ED_screens_init(G_MAIN, CTX_wm_manager(C));

  } while (exitrequested == KX_ExitRequest::RESTART_GAME ||
           exitrequested == KX_ExitRequest::START_OTHER_GAME);

  if (bfd) {
    /* Hack to not free the win->ghosting AND win->gpu_ctx when we restart/load new
     * .blend */
    CTX_wm_window(C)->ghostwin = nullptr;
    /* Hack to not free wm->message_bus when we restart/load new .blend */
    CTX_wm_manager(C)->message_bus = nullptr;
    BLO_blendfiledata_free(bfd);

    /* Restore Main and Scene used before ge start */
    G_MAIN = G.main = maggie1;
    CTX_data_main_set(C, maggie1);
    CTX_wm_manager_set(C, wm_backup);
    CTX_wm_window_set(C, win_backup);
    win_backup->ghostwin = ghostwin_backup;
    win_backup->gpuctx = gpuctx_backup;
    wm_backup->message_bus = (wmMsgBus *)msgbus_backup;
    InitBlenderContextVariables(C, wm_backup, startscene);
    WM_check(C);
  }

  /* Undo System */
  if (startscene->gm.flag & GAME_USE_UNDO) {
    UndoStep *step_data_from_name = NULL;
    step_data_from_name = BKE_undosys_step_find_by_name(CTX_wm_manager(C)->undo_stack,
                                                        "bge_start");
    if (step_data_from_name) {
      BKE_undosys_step_undo_with_data(CTX_wm_manager(C)->undo_stack, C, step_data_from_name);
    }
    else {
      BKE_undosys_step_undo(CTX_wm_manager(C)->undo_stack, C);
    }
  }

#ifdef WITH_PYTHON

  PyDict_Clear(globalDict);
  Py_DECREF(globalDict);

  // Release Python's GIL
  PyGILState_Release(gilstate);
#endif

  // Restore G_MAIN path.
  BLI_strncpy(G_MAIN->name, prevPathName, sizeof(G_MAIN->name));
}
