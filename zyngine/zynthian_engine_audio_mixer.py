#!/usr/bin/python3
# -*- coding: utf-8 -*-
# ********************************************************************
# ZYNTHIAN PROJECT: Zynmixer Python Wrapper
#
# A Python wrapper for zynmixer library
#
# Copyright (C) 2019-2024 Brian Walton <riban@zynthian.org>
#
# ********************************************************************
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
# ********************************************************************

import logging

from zyngine import zynthian_engine
from zyngine import zynthian_controller
from zyngine.zynthian_signal_manager import zynsigman
from zynlibs.zynmixer.zynmixer import SS_ZYNMIXER_SET_VALUE

import zynautoconnect

# -------------------------------------------------------------------------------
# zynmixer channel strip engine
# -------------------------------------------------------------------------------

class zynthian_engine_audio_mixer(zynthian_engine):

    # Subsignals are defined inside each module. Here we define audio_mixer subsignals:
    SS_ZYNMIXER_SET_VALUE = 1

    # Controller Screens
    _ctrl_screens = [
        ['main', ['level', 'balance', 'mute', 'solo']],
        ['aux', ['mono', 'phase', 'ms', 'record']]
    ]

    # Function to initialize library
    def __init__(self, state_manager):
        super().__init__(state_manager)
        self.type = "Audio Effect"
        self.name = "AudioMixer"
        self.MAX_NUM_CHANNELS = 0

    def start(self):
        pass

    def get_controllers_dict(self, processor):
        if not processor.controllers_dict:
            processor.controllers_dict = {
                'level': zynthian_controller(self, 'level', {
                    'is_integer': False,
                    'value_max': 1.0,
                    'value_default': 0.8,
                    'value': processor.zynmixer.get_level(processor.mixer_chan),
                    'processor': processor
                }),
                'balance': zynthian_controller(self, 'balance', {
                    'is_integer': False,
                    'value_min': -1.0,
                    'value_max': 1.0,
                    'value_default': 0.0,
                    'value': processor.zynmixer.get_balance(processor.mixer_chan),
                    'processor': processor
                }),
                'mute': zynthian_controller(self, 'mute', {
                    'is_toggle': True,
                    'value_max': 1,
                    'value_default': 0,
                    'value': processor.zynmixer.get_mute(processor.mixer_chan),
                    'processor': processor,
                    'labels': ['off', 'on']
                }),
                'solo': zynthian_controller(self, 'solo', {
                    'is_toggle': True,
                    'value_max': 1,
                    'value_default': 0,
                    'value': 0,
                    'processor': processor,
                    'labels': ['off', 'on']
                }),
                'mono': zynthian_controller(self, 'mono', {
                    'is_toggle': True,
                    'value_max': 1,
                    'value_default': 0,
                    'value': processor.zynmixer.get_mono(processor.mixer_chan),
                    'processor': processor,
                    'labels': ['off', 'on']
                }),
                'ms': zynthian_controller(self, 'ms', {
                    'is_toggle': True,
                    'value_max': 1,
                    'value_default': 0,
                    'value': processor.zynmixer.get_ms(processor.mixer_chan),
                    'labels': ['off', 'on'],
                    'processor': processor,
                    'name': "M+S"
                }),
                'phase': zynthian_controller(self, 'phase', {
                    'is_toggle': True,
                    'value_max': 1,
                    'value_default': 0,
                    'value': processor.zynmixer.get_phase(processor.mixer_chan),
                    'processor': processor,
                    'labels': ['off', 'on']
                }),
                'record': zynthian_controller(self, 'record', {
                    'is_toggle': True,
                    'value_max': 1,
                    'value_default': 0,
                    'value': 0,
                    'processor': processor,
                    'labels': ['off', 'on']
                })
            }
        return processor.controllers_dict

    def refresh_fx_send(self):
        send_count = self.state_manager.zynmixer_chan.get_send_count()
        for processor in self.processors:
            if processor.zynmixer == self.state_manager.zynmixer_bus:
                continue
            send = 0
            while True:
                symbol = f"send_{send}"
                mode_symbol = f"send_{send}_mode"
                if send < send_count:
                    # Check that processor has send control
                    if symbol not in processor.controllers_dict:
                        processor.controllers_dict[symbol] = zynthian_controller(self, symbol, {
                            'name': f'send {send + 1} level',
                            'value_max': 1.0,
                            'value_default': 0.0,
                            'value': processor.zynmixer.get_send(processor.mixer_chan, send),
                            'processor': processor,
                            'graph_path': ["send", send]
                        })
                        processor.controllers_dict[mode_symbol] = zynthian_controller(self, mode_symbol, {
                            'name': f'send {send + 1} mode',
                            'value_max': 1,
                            'value_default': 0,
                            'value': processor.zynmixer.get_send(processor.mixer_chan, send),
                            'labels': ['post fader', 'pre fader'],
                            'processor': processor,
                            'graph_path': ["send_mode", send]
                        })
                        processor.ctrl_screens_dict[f"send {send + 1}"] = [processor.controllers_dict[symbol], processor.controllers_dict[f"{symbol}_mode"]]
                else:
                    # Check that processor does not have send control
                    try:
                        del processor.controllers_dict[symbol]
                        del processor.controllers_dict[mode_symbol]
                        del processor.ctrl_screens_dict[f"send {send}"]
                    except:
                        break
                send += 1

    def add_processor(self, processor):
        self.processors.append(processor)
        if processor.engine.mixbus:
            processor.zynmixer = self.state_manager.zynmixer_bus
            processor.jackname = "zynmixer_bus"
            if processor.chain_id:
                # FX chain
                processor.mixer_chan = self.state_manager.zynmixer_bus.add_strip()
                send = self.state_manager.zynmixer_chan.add_send()
                if processor.mixer_chan != send:
                    logging.warning("FX send/return index mismatch")
                processor.name = f"Effect Return {self.state_manager.zynmixer_chan.get_send_count()}"
            else:
                # Main mixbus
                processor.mixer_chan = 0
                processor.name = "Main Mixbus"
        else:
            # Normal audio mixer strip
            processor.zynmixer = self.state_manager.zynmixer_chan
            processor.jackname = "zynmixer_chan"
            processor.mixer_chan = self.state_manager.zynmixer_chan.add_strip()
            processor.name = f"Mixer Channel Strip {processor.mixer_chan + 1}"
        processor.refresh_controllers()
        self.refresh_fx_send()
        return

    def remove_processor(self, processor):
        processor.zynmixer.set_mute(processor.mixer_chan, 1)
        super().remove_processor(processor)
        processor.zynmixer.remove_strip(processor.mixer_chan)
        if processor.zynmixer == self.state_manager.zynmixer_bus:
            send = processor.mixer_chan
            self.state_manager.zynmixer_chan.remove_send(send)
            self.refresh_fx_send()

    def set_extended_config(self, config):
        if config is None:
            return
        if "mixbus" in config:
            self.mixbus = config["mixbus"]

    def send_controller_value(self, zctrl):
        try:
            if zctrl.symbol.startswith("send"):
                getattr(zctrl.processor.zynmixer, f'set_{zctrl.graph_path[0]}')(
                    zctrl.processor.mixer_chan, zctrl.graph_path[1], zctrl.value)
            elif zctrl.symbol == "record":
                #TODO: Use jackname to arm
                self.state_manager.audio_recorder.arm(zctrl.processor, zctrl.value)
            elif zctrl.symbol == "solo":
                if zctrl.processor.chain_id == 0 and zctrl.value == 1:
                    for processor in self.processors:
                        processor.controllers_dict["solo"].set_value(0, False)
                        zynautoconnect.solo(processor.chain_id, 0)
                else:
                    zynautoconnect.solo(zctrl.processor.chain_id, zctrl.value)
                zynautoconnect.request_audio_connect(True)
                zynsigman.send(zynsigman.S_AUDIO_MIXER, SS_ZYNMIXER_SET_VALUE,
                    mixbus=zctrl.processor.zynmixer.mixbus,
                    channel=zctrl.processor.mixer_chan,
                    symbol="solo", value=zctrl.value)
            else:
                getattr(zctrl.processor.zynmixer, f'set_{zctrl.symbol}')(
                    zctrl.processor.mixer_chan, zctrl.value)
        except Exception as e:
            logging.warning(e)

    def get_path(self, processor):
        return processor.name
        if processor.chain_id:
            if processor.mixbus:
                return f"FX Send {processor.chain_id}"
            else:
                return f"Mixer Channel {processor.chain_id}"
        return f"Main Mixbus"


# -------------------------------------------------------------------------------
