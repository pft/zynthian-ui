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

import ctypes
import logging

from zyngine import zynthian_engine
from zyngine import zynthian_controller
from zyngine.zynthian_signal_manager import zynsigman

# -------------------------------------------------------------------------------
# Zynmixer Library Wrapper and processor
# -------------------------------------------------------------------------------

class zynthian_engine_audio_mixer(zynthian_engine):

    # Subsignals are defined inside each module. Here we define audio_mixer subsignals:
    SS_ZCTRL_SET_VALUE = 1

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
        self.lib_zynmixer = ctypes.cdll.LoadLibrary(
            "/zynthian/zynthian-ui/zynlibs/zynmixer/build/libzynmixer.so")
        self.midi_learn_zctrl = None
        self.fx_loop = False # Used as temporary flag when adding processor (bodge)

        self.lib_zynmixer.addStrip.argtypes = [ctypes.c_uint8]
        self.lib_zynmixer.addStrip.restype = ctypes.c_int8
        self.lib_zynmixer.removeStrip.argtypes = [ctypes.c_uint8]
        self.lib_zynmixer.removeStrip.restype = ctypes.c_int8

        self.lib_zynmixer.setLevel.argtypes = [ctypes.c_uint8, ctypes.c_float]
        self.lib_zynmixer.getLevel.argtypes = [ctypes.c_uint8]
        self.lib_zynmixer.getLevel.restype = ctypes.c_float

        self.lib_zynmixer.setBalance.argtypes = [
            ctypes.c_uint8, ctypes.c_float]
        self.lib_zynmixer.getBalance.argtypes = [ctypes.c_uint8]
        self.lib_zynmixer.getBalance.restype = ctypes.c_float

        self.lib_zynmixer.setMute.argtypes = [ctypes.c_uint8, ctypes.c_uint8]
        self.lib_zynmixer.toggleMute.argtypes = [ctypes.c_uint8]
        self.lib_zynmixer.getMute.argtypes = [ctypes.c_uint8]
        self.lib_zynmixer.getMute.restype = ctypes.c_uint8

        self.lib_zynmixer.setMS.argtypes = [ctypes.c_uint8, ctypes.c_uint8]
        self.lib_zynmixer.getMS.argtypes = [ctypes.c_uint8]
        self.lib_zynmixer.getMS.restypes = ctypes.c_uint8

        self.lib_zynmixer.setSolo.argtypes = [ctypes.c_uint8, ctypes.c_uint8]
        self.lib_zynmixer.getSolo.argtypes = [ctypes.c_uint8]
        self.lib_zynmixer.getSolo.restype = ctypes.c_uint8

        self.lib_zynmixer.setMono.argtypes = [ctypes.c_uint8, ctypes.c_uint8]
        self.lib_zynmixer.getMono.argtypes = [ctypes.c_uint8]
        self.lib_zynmixer.getMono.restype = ctypes.c_uint8

        self.lib_zynmixer.setPhase.argtypes = [ctypes.c_uint8, ctypes.c_uint8]
        self.lib_zynmixer.getPhase.argtypes = [ctypes.c_uint8]
        self.lib_zynmixer.getPhase.restype = ctypes.c_uint8

        self.lib_zynmixer.setSend.argtypes = [ctypes.c_uint8, ctypes.c_uint8, ctypes.c_float]
        self.lib_zynmixer.getSend.argtypes = [ctypes.c_uint8, ctypes.c_uint8]
        self.lib_zynmixer.getSend.restype = ctypes.c_float

        self.lib_zynmixer.setNormalise.argtypes = [
            ctypes.c_uint8, ctypes.c_uint8]
        self.lib_zynmixer.getNormalise.argtypes = [ctypes.c_uint8]
        self.lib_zynmixer.getNormalise.restype = ctypes.c_uint8

        self.lib_zynmixer.reset.argtypes = [ctypes.c_uint8]

        self.lib_zynmixer.getDpm.argtypes = [ctypes.c_uint8, ctypes.c_uint8]
        self.lib_zynmixer.getDpm.restype = ctypes.c_float

        self.lib_zynmixer.getDpmHold.argtypes = [
            ctypes.c_uint8, ctypes.c_uint8]
        self.lib_zynmixer.getDpmHold.restype = ctypes.c_float

        self.lib_zynmixer.getDpmStates.argtypes = [
            ctypes.c_uint8, ctypes.c_uint8, ctypes.POINTER(ctypes.c_float)]

        self.lib_zynmixer.enableDpm.argtypes = [
            ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8]

        self.lib_zynmixer.getMaxChannels.restype = ctypes.c_uint8

        self.MAX_NUM_CHANNELS = self.lib_zynmixer.getMaxChannels()

        # List of learned {cc:zctrl} indexed by learned MIDI channel
        self.learned_cc = [dict() for x in range(16)]

    def stop(self):
        #TODO: Implement stop
        pass

    def get_controllers_dict(self, processor):
        if not processor.controllers_dict:
            processor.controllers_dict = {
                'level': zynthian_controller(self, 'level', {
                    'is_integer': False,
                    'value_max': 1.0,
                    'value_default': 0.8,
                    'value': self.get_level(processor.mixer_chan),
                    'processor': processor,
                    'graph_path': [processor.mixer_chan, 'level']
                }),
                'balance': zynthian_controller(self, 'balance', {
                    'is_integer': False,
                    'value_min': -1.0,
                    'value_max': 1.0,
                    'value_default': 0.0,
                    'value': self.get_balance(processor.mixer_chan),
                    'processor': processor,
                    'graph_path': [processor.mixer_chan, 'balance']
                }),
                'mute': zynthian_controller(self, 'mute', {
                    'is_toggle': True,
                    'value_max': 1,
                    'value_default': 0,
                    'value': self.get_mute(processor.mixer_chan),
                    'graph_path': [processor.mixer_chan, 'mute'],
                    'processor': processor,
                    'labels': ['off', 'on']
                }),
                'solo': zynthian_controller(self, 'solo', {
                    'is_toggle': True,
                    'value_max': 1,
                    'value_default': 0,
                    'value': self.get_solo(processor.mixer_chan),
                    'graph_path': [processor.mixer_chan, 'solo'],
                    'processor': processor,
                    'labels': ['off', 'on']
                }),
                'mono': zynthian_controller(self, 'mono', {
                    'is_toggle': True,
                    'value_max': 1,
                    'value_default': 0,
                    'value': self.get_mono(processor.mixer_chan),
                    'graph_path': [processor.mixer_chan, 'mono'],
                    'processor': processor,
                    'labels': ['off', 'on']
                }),
                'ms': zynthian_controller(self, 'ms', {
                    'is_toggle': True,
                    'value_max': 1,
                    'value_default': 0,
                    'value': self.get_ms(processor.mixer_chan),
                    'graph_path': [processor.mixer_chan, 'ms'],
                    'labels': ['off', 'on'],
                    'processor': processor,
                    'name': "M+S"
                }),
                'phase': zynthian_controller(self, 'phase', {
                    'is_toggle': True,
                    'value_max': 1,
                    'value_default': 0,
                    'value': self.get_phase(processor.mixer_chan),
                    'graph_path': [processor.mixer_chan, 'phase'],
                    'processor': processor,
                    'labels': ['off', 'on']
                }),
                'record': zynthian_controller(self, 'record', {
                    'is_toggle': True,
                    'value_max': 1,
                    'value_default': 0,
                    'value': 0,
                    'graph_path': [processor.mixer_chan, 'record'],
                    'processor': processor,
                    'labels': ['off', 'on']
                })
            }
        return processor.controllers_dict

    def update_fx_send(self, processor, send):
        symbol = f"send_{send:02d}"
        processor.controllers_dict[symbol] = zynthian_controller(self, symbol, {
            'name': f'send {send} level',
            'value_max': 1.0,
            'value_default': 0.0,
            'value': self.get_send(processor.mixer_chan, send),
            'graph_path': [processor.mixer_chan, "send", send]
        })
        processor.ctrl_screens_dict[f"send {send}"] = [processor.controllers_dict[symbol]]

    def add_processor(self, processor):
        self.processors.append(processor)
        processor.fx_loop = processor.engine.fx_loop
        if processor.chain_id:
            processor.mixer_chan = self.add_strip(processor.fx_loop)
        else:
            processor.mixer_chan = 0
        if processor.fx_loop or processor.chain_id == 0:
            processor.jackname = "zynmixer_buses"
        else:
            processor.jackname = "zynmixer_chans"
        processor.refresh_controllers()
        if not processor.fx_loop:
            for proc in self.processors:
                if proc.chain_id and proc.fx_loop:
                    self.update_fx_send(processor, proc.mixer_chan)
        return

    def remove_processor(self, processor):
        super().remove_processor(processor)
        self.lib_zynmixer.removeStrip(processor.mixer_chan)

    def set_extended_config(self, config):
        if config is None:
            return
        if "fx_loop" in config:
            self.fx_loop = config["fx_loop"]

    def send_controller_value(self, zctrl):
        try:
            if zctrl.graph_path[1] == "send":
                self.set_send(zctrl.graph_path[0], zctrl.graph_path[2], zctrl.value, False)
            elif zctrl.symbol == "record":
                if zctrl.value:
                    self.state_manager.audio_recorder.arm(zctrl.graph_path[0])
                else:
                    self.state_manager.audio_recorder.unarm(zctrl.graph_path[0])
            else:
                getattr(self, f'set_{zctrl.symbol}')(
                    zctrl.graph_path[0], zctrl.value, False)
        except Exception as e:
            logging.warning(e)

    def add_strip(self, fx_loop=False):
        strip = self.lib_zynmixer.addStrip(fx_loop)
        if fx_loop:
            for proc in self.processors:
                if not proc.fx_loop:
                    self.update_fx_send(proc, strip)                    
        return strip

    def remove_strip(self, chan):
        return self.lib_zynmixer.removeStrip(chan)

    def get_path(self, processor):
        if processor.chain_id:
            if processor.fx_loop:
                return f"FX Send {processor.chain_id}"
            else:
                return f"Mixer Channel {processor.chain_id}"
        return f"Main Mixbus"

    # Function to set fader level for a channel
    # channel: Index of channel
    # level: Fader value (0..+1)
    # update: True for update controller

    def set_level(self, channel, level, update=True):
        if channel is None:
            return
        self.lib_zynmixer.setLevel(channel, ctypes.c_float(level))
        if update:
            self.zctrls[channel]['level'].set_value(level, False)
        zynsigman.send(zynsigman.S_AUDIO_MIXER, self.SS_ZCTRL_SET_VALUE,
                       chan=channel, symbol="level", value=level)

    # Function to get fader level for a channel
    # channel: Index of channel
    # returns: Fader level (0..+1)
    def get_level(self, channel):
        if channel is None:
            return
        return self.lib_zynmixer.getLevel(channel)

    # Function to set balance for a channel
    # channel: Index of channel
    # balance: Balance value (-1..+1)
    # update: True for update controller
    def set_balance(self, channel, balance, update=True):
        if channel is None:
            return
        self.lib_zynmixer.setBalance(channel, ctypes.c_float(balance))
        if update:
            self.zctrls[channel]['balance'].set_value(balance, False)
        zynsigman.send(zynsigman.S_AUDIO_MIXER, self.SS_ZCTRL_SET_VALUE,
                       chan=channel, symbol="balance", value=balance)

    # Function to get balance for a channel
    # channel: Index of channel
    # returns: Balance value (-1..+1)
    def get_balance(self, channel):
        if channel is None:
            return
        return self.lib_zynmixer.getBalance(channel)

    # Function to set mute for a channel
    # channel: Index of channel
    # mute: Mute state (True to mute)
    # update: True for update controller
    def set_mute(self, channel, mute, update=False):
        if channel is None:
            return
        self.lib_zynmixer.setMute(channel, mute)
        if update:
            self.zctrls[channel]['mute'].set_value(mute, False)
        zynsigman.send(zynsigman.S_AUDIO_MIXER, self.SS_ZCTRL_SET_VALUE,
                       chan=channel, symbol="mute", value=mute)

    # Function to get mute for a channel
    # channel: Index of channel
    # returns: Mute state (True if muted)
    def get_mute(self, channel, update=False):
        if channel is None:
            return
        return self.lib_zynmixer.getMute(channel)

    # Function to toggle mute of a channel
    # channel: Index of channel
    # update: True for update controller
    def toggle_mute(self, channel, update=False):
        self.lib_zynmixer.toggleMute(channel)
        mute = self.lib_zynmixer.getMute(channel)
        if update:
            self.zctrls[channel]['mute'].set_value(mute, False)
        zynsigman.send(zynsigman.S_AUDIO_MIXER, self.SS_ZCTRL_SET_VALUE,
                       chan=channel, symbol="mute", value=mute)

    # Function to set phase reversal for a channel
    # channel: Index of channel
    # phase: Phase reversal state (True to reverse)
    # update: True for update controller
    def set_phase(self, channel, phase, update=True):
        if channel is None:
            return
        self.lib_zynmixer.setPhase(channel, phase)
        if update:
            self.zctrls[channel]['phase'].set_value(phase, False)
        zynsigman.send(zynsigman.S_AUDIO_MIXER, self.SS_ZCTRL_SET_VALUE,
                       chan=channel, symbol="phase", value=phase)

    # Function to get phase reversal for a channel
    # channel: Index of channel
    # returns: Phase reversal state (True if phase reversed)
    def get_phase(self, channel):
        if channel is None:
            return
        return self.lib_zynmixer.getPhase(channel)

    # Function to toggle phase reversal of a channel
    # channel: Index of channel
    # update: True for update controller
    def toggle_phase(self, channel, update=True):
        if channel is None:
            return
        self.lib_zynmixer.togglePhase(channel)
        phase = self.lib_zynmixer.getPhase(channel)
        if update:
            self.zctrls[channel]['phase'].set_value(phase, False)
        zynsigman.send(zynsigman.S_AUDIO_MIXER, self.SS_ZCTRL_SET_VALUE,
                       chan=channel, symbol="phase", value=phase)

    # Function to set solo for a channel
    # channel: Index of channel
    # solo: Solo state (True to solo)
    # update: True for update controller
    def set_solo(self, channel, solo, update=True):
        if channel is None:
            return
        self.lib_zynmixer.setSolo(channel, solo)
        if update:
            #TODO: Get correct channel/processor
            self.processors[channel].controllers_dict['solo'].set_value(solo, False)
        if channel:
            zynsigman.send(zynsigman.S_AUDIO_MIXER, self.SS_ZCTRL_SET_VALUE,
                       chan=channel, symbol="solo", value=solo)
        else:
            # Main strip solo clears all chain solo
            for proc in self.processors:
                proc.controllers_dict["solo"].set_value(0, False)
                zynsigman.send(
                    zynsigman.S_AUDIO_MIXER, self.SS_ZCTRL_SET_VALUE, chan=proc.mixer_chan, symbol="solo", value=0)

    # Function to get solo for a channel
    # channel: Index of channel
    # returns: Solo state (True if solo)
    def get_solo(self, channel):
        if channel is None:
            return
        return self.lib_zynmixer.getSolo(channel) == 1

    # Function to toggle mute of a channel
    # channel: Index of channel
    # update: True for update controller
    def toggle_solo(self, channel, update=True):
        if channel is None:
            return
        if self.get_solo(channel):
            self.set_solo(channel, False)
        else:
            self.set_solo(channel, True)

    # Function to mono a channel
    # channel: Index of channel
    # mono: Mono state (True to solo)
    # update: True for update controller
    def set_mono(self, channel, mono, update=True):
        if channel is None:
            return
        self.lib_zynmixer.setMono(channel, mono)
        if update:
            self.zctrls[channel]['mono'].set_value(mono, False)
        zynsigman.send(zynsigman.S_AUDIO_MIXER, self.SS_ZCTRL_SET_VALUE,
                       chan=channel, symbol="mono", value=mono)

    # Function to get mono for a channel
    # channel: Index of channel
    # returns: Mono state (True if mono)
    def get_mono(self, channel):
        if channel is None:
            return
        return self.lib_zynmixer.getMono(channel) == 1

    # Function to get all mono
    # returns: List of mono states (True if mono)
    def get_all_monos(self):
        monos = (ctypes.c_bool * (self.MAX_NUM_CHANNELS))()
        self.lib_zynmixer.getAllMono(monos)
        result = []
        for i in monos:
            result.append(i)
        return result

    # Function to toggle mono of a channel
    # channel: Index of channel
    # update: True for update controller
    def toggle_mono(self, channel, update=True):
        if channel is None:
            return
        if self.get_mono(channel):
            self.set_mono(channel, False)
        else:
            self.set_mono(channel, True)
        if update:
            self.zctrls[channel]['mono'].set_value(
                self.lib_zynmixer.getMono(channel), False)

    # Function to enable M+S mode
    # channel: Index of channel
    # enable: M+S state (True to enable)
    # update: True for update controller

    def set_ms(self, channel, enable, update=True):
        if channel is None:
            return
        self.lib_zynmixer.setMS(channel, enable)
        if update:
            self.zctrls[channel]['ms'].set_value(enable, False)
        zynsigman.send(zynsigman.S_AUDIO_MIXER, self.SS_ZCTRL_SET_VALUE,
                       chan=channel, symbol="ms", value=enable)

    # Function to get M+S mode
    # channel: Index of channel
    # returns: M+S mode (True if enabled)
    def get_ms(self, channel):
        if channel is None:
            return
        return self.lib_zynmixer.getMS(channel) == 1

    # Function to toggle M+S mode
    # channel: Index of channel
    # update: True for update controller
    def toggle_ms(self, channel, update=True):
        if channel is None:
            return
        if self.get_ms(channel):
            self.set_ms(channel, False)
        else:
            self.set_ms(channel, True)
        if update:
            self.zctrls[channel]['ms'].set_value(
                self.lib_zynmixer.getMS(channel), False)

    # Function to set fx send level for a channel
    # channel: Index of channel
    # send: Index of fx send
    # level: Fader value (0..+1)
    # update: True for update controller

    def set_send(self, channel, send, level, update=True):
        if channel is None or send is None:
            return
        self.lib_zynmixer.setSend(channel, send, ctypes.c_float(level))
        if update:
            self.zctrls[channel][f'send_{send:02d}'].set_value(level, False)
        zynsigman.send(zynsigman.S_AUDIO_MIXER, self.SS_ZCTRL_SET_VALUE,
                       chan=channel, symbol=f"send_{send:02d}", value=level)

    # Function to get fx send level for a channel
    # channel: Index of channel
    # send: Index of fx send
    # returns: Send level (0..+1)
    def get_send(self, channel, send):
        if channel is None or send is None:
            return
        return self.lib_zynmixer.getSend(channel, send)

    # Function to set internal normalisation of a channel when its direct output is not routed
    # channel: Index of channel
    # enable: True to enable internal normalisation
    def normalise(self, channel, enable):
        if channel is None:
            return
        if channel == 0:
            return  # Don't allow normalisation of main mixbus (to itself)
        self.lib_zynmixer.setNormalise(channel, enable)

    # Function to get the internal normalisation state of s channel
    # channel: Index of channel
    # enable: True to enable internal normalisation
    # update: True for update controller
    def is_normalised(self, channel):
        if channel is None:
            return False
        return self.lib_zynmixer.getNormalise(channel)

    # Function to get peak programme level for a channel
    # channel: Index of channel
    # leg: 0 for A-leg (left), 1 for B-leg (right)
    # returns: Peak programme level
    def get_dpm(self, channel, leg):
        if channel is None:
            return
        return self.lib_zynmixer.getDpm(channel, leg)

    # Function to get peak programme hold level for a channel
    # channel: Index of channel
    # leg: 0 for A-leg (left), 1 for B-leg (right)
    # returns: Peak programme hold level
    def get_dpm_holds(self, channel, leg):
        if channel is None:
            return
        return self.lib_zynmixer.getDpmHold(channel, leg)

    # Function to get the dpm states for a set of channels
    # start: Index of first channel
    # end: Index of last channel
    # returns: List of tuples containing (dpm_a, dpm_b, hold_a, hold_b, mono)
    def get_dpm_states(self, start, end):
        state = (ctypes.c_float * (5 * (end - start + 1)))()
        self.lib_zynmixer.getDpmStates(start, end, state)
        result = []
        offset = 0
        for channel in range(start, end + 1):
            l = []
            for i in range(4):
                l.append(state[offset])
                offset += 1
            l.append(state[offset] != 0.0)
            offset += 1
            result.append(l)
        return result

    # Function to enable or disable digital peak meters
    # start: First mixer channel
    # end: Last mixer channel
    # enable: True to enable
    def enable_dpm(self, start, end, enable):
        if start is None or end is None:
            return
        self.lib_zynmixer.enableDpm(start, end, int(enable))

    # Function to add OSC client registration
    # client: IP address of OSC client
    def add_osc_client(self, client):
        return self.lib_zynmixer.addOscClient(ctypes.c_char_p(client.encode('utf-8')))

    # Function to remove OSC client registration
    # client: IP address of OSC client
    def remove_osc_client(self, client):
        self.lib_zynmixer.removeOscClient(
            ctypes.c_char_p(client.encode('utf-8')))

    def reset(self):
        for channel in range(self.MAX_NUM_CHANNELS):
            self.lib_zynmixer.reset(channel)

# -------------------------------------------------------------------------------
