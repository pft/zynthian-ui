#!/usr/bin/python3
# -*- coding: utf-8 -*-
# ******************************************************************************
# ZYNTHIAN PROJECT: Zynthian GUI
#
# Zynthian GUI Audio-Out Selector Class
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

import logging

# Zynthian specific modules
import zynautoconnect
from zyngui.zynthian_gui_selector import zynthian_gui_selector

# ------------------------------------------------------------------------------
# Zynthian Audio-Out Selection GUI Class
# ------------------------------------------------------------------------------


class zynthian_gui_audio_out(zynthian_gui_selector):

    def __init__(self):
        self.chain = None
        super().__init__('Audio Out', True)

    def set_chain(self, chain):
        self.chain = chain

    def fill_list(self):
        self.list_data = []
        if self.chain.chain_id:
            # Normal chain so add mixer / chain targets
            port_names = [("Main mixbus", 0)]
            self.list_data.append((None, None, "> Chain inputs"))
            for chain_id, chain in self.zyngui.chain_manager.chains.items():
                if chain_id != 0 and chain != self.chain and chain.audio_thru or chain.is_synth() and chain.synth_slots[0][0].type == "Special":
                    if self.zyngui.chain_manager.will_audio_howl(self.chain.chain_id, chain_id):
                        prefix = "∞ "
                    else:
                        prefix = ""
                    port_names.append((f"{prefix}{chain.get_name()}", chain_id))
                # Add side-chain targets
                for processor in chain.get_processors():
                    try:
                        for port_name in zynautoconnect.get_sidechain_portnames(processor.jackname):
                            port_names.append((f"↣ side {port_name}", port_name))
                    except:
                        pass
            for title, processor in port_names:
                if processor in self.chain.audio_out:
                    self.list_data.append((processor, processor, "\u2612 " + title))
                else:
                    self.list_data.append((processor, processor, "\u2610 " + title))

        if self.chain.is_audio():
            port_names = []
            # Direct physical outputs
            self.list_data.append((None, None, "> Direct Outputs"))
            ports = zynautoconnect.get_hw_audio_dst_ports()
            port_count = len(ports)
            for i in range(1, port_count + 1, 2):
                if i < port_count:
                    port_names.append((f"Output {i}", f"system:playback_{i}$"))
                    port_names.append((f"Output {i + 1}", f"system:playback_{i + 1}$"))
                    port_names.append((f"Outputs {i}+{i + 1}", f"system:playback_[{i},{i + 1}]$"))
                else:
                    port_names.append((f"Output {i}", f"system:playback_{i}$"))
            for title, processor in port_names:
                if processor in self.chain.audio_out:
                    self.list_data.append((processor, processor, "\u2612 " + title))
                else:
                    self.list_data.append((processor, processor, "\u2610 " + title))

        super().fill_list()

    def fill_listbox(self):
        super().fill_listbox()

    def select_action(self, i, t='S'):
        self.chain.toggle_audio_out(self.list_data[i][0])
        self.fill_list()

    def set_select_path(self):
        self.select_path.set("Send Audio to ...")

# ------------------------------------------------------------------------------
