# -*- coding: utf-8 -*-
# ******************************************************************************
# ZYNTHIAN PROJECT: Zynthian GUI
#
# Zynthian GUI Help view class
#
# Copyright (C) 2015-2024 Fernando Moyano <jofemodo@zynthian.org>
#
# ******************************************************************************
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License as
# published by the Free Software Foundation; either version 2 of
# the License, or any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# For a full copy of the GNU General Public License see the LICENSE.txt file.
#
# ******************************************************************************

import os
import logging
from tkinterweb import HtmlFrame

# Zynthian specific modules
from zyngui import zynthian_gui_config

# ------------------------------------------------------------------------------
# Zynthian help view GUI Class
# ------------------------------------------------------------------------------


class zynthian_gui_help:

    ui_dir = os.environ.get('ZYNTHIAN_UI_DIR', "/zynthian/zynthian-ui")

    def __init__(self):
        self.shown = False
        self.zyngui = zynthian_gui_config.zyngui
        # Main Frame
        self.main_frame = HtmlFrame(zynthian_gui_config.top, messages_enabled=False)

    def load_file(self, fpath):
        if os.path.isfile(fpath):
            try:
                self.main_frame.load_file("file:///" + self.ui_dir + "/" + fpath, force=True, insecure=True)
                return True
            except Exception as e:
                logging.error(f"Can't load HTML file => {e}")
        return False

    def build_view(self):
        return True

    def hide(self):
        if self.shown:
            self.shown = False
            self.main_frame.grid_forget()

    def show(self):
        if self.zyngui.test_mode:
            logging.warning("TEST_MODE: {}".format(self.__class__.__module__))
        if not self.shown:
            self.shown = True
            self.main_frame.grid()

    def zynpot_cb(self, i, dval):
        if i == 3:
            self.main_frame.yview_scroll(dval, "units")
        return True

    def refresh_loading(self):
        pass

    def switch_select(self, t='S'):
        pass

    def cb_push(self, event):
        self.zyngui.cuia_back()

# -------------------------------------------------------------------------------
