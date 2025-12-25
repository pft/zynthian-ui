# -*- coding: utf-8 -*-
# ******************************************************************************
# ZYNTHIAN PROJECT: Zynthian Engine (zynthian_engine_inetradio)
#
# zynthian_engine implementation for internet radio streamer
#
# Copyright (C) 2022-2025 Brian Walton <riban@zynthian.org>
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

from collections import OrderedDict
import logging
import json
import copy
from subprocess import Popen, STDOUT, PIPE
import socket
from threading import Thread, Timer
from os.path import basename
from os import listdir
from time import sleep, monotonic
import dbus
import jack

from . import zynthian_engine
import zynautoconnect
import dbus.mainloop.glib
from gi.repository import GLib

# Source - https://stackoverflow.com/a
# Posted by Marquinho Peli
# Retrieved 2025-12-04, License - CC BY-SA 4.0
def debounce(wait_time):
    """
    Decorator that will debounce a function so that it is called after wait_time seconds
    If it is called multiple times, will wait for the last call to be debounced and run only this one.
    """

    def decorator(function):
        def debounced(*args, **kwargs):
            def call_function():
                debounced._timer = None
                return function(*args, **kwargs)
            # if we already have a call to the function currently waiting to be executed, reset the timer
            if debounced._timer is not None:
                debounced._timer.cancel()

            # after wait_time, call the function provided to the decorator with its arguments
            debounced._timer = Timer(wait_time, call_function)
            debounced._timer.start()

        debounced._timer = None
        return debounced

    return decorator

# ------------------------------------------------------------------------------
# Internet Radio Engine Class
# ------------------------------------------------------------------------------

#MAGIC_STRING = '[INFO] Running "/bin/true" using "/bin/bash" with environment variables '
MAGIC_STRING = '[\x1b[37mINFO\x1b[0m] Running "/bin/true" using "/bin/bash" with environment variables '
MAGIC_STRING_LEN = len(MAGIC_STRING)

class zynthian_engine_librespot(zynthian_engine):

    default_presets = {
        "Ambient": [
            ["http://relax.stream.publicradio.org/relax.mp3",
             0, "Relax"]        
        ]
    }

    # ---------------------------------------------------------------------------
    # Config variables
    # ---------------------------------------------------------------------------

    # ---------------------------------------------------------------------------
    # Initialization
    # ---------------------------------------------------------------------------

    def __init__(self, zyngui=None):
        super().__init__(zyngui)
        self.name = "LibreSpot"
        self.nickname = "SP"
        self.jackname = "cpal_client_out"
        self.type = "Audio Generator"
        self.track_id = None
        self.presets = None
        self.banks = None
        self.preset = None  # Currently selected preset
        self.preset2bank = []  # List of (bank index, preset index, preset name) for prev/next optimisation
        self.preset_i = 0  # Index of current preset in preset2bank list
        self.pending_preset_i = 0  # Index of preselected pending preset
        self.pending_preset_ts = 0  # Timeout to select pending preset
        self.client = None  # Telnet client to vlc
        self.connect_timer = None  # Timer to trigger autoconnect
        self.proc_poll_thread = None
        self.position_thread = None
        self.monitors_dict = {
            'title': "",
            'artist': "",
            'info': "",
            'channels': "",
            'codec': "",
            'bitrate': "",
            'url': "",
            'reset': False,
            'artwork': "",
            'connected': False,
            'length': 0
        }
        self.custom_gui_fpath = "/zynthian/zynthian-ui/zyngui/zynthian_widget_inet_radio.py"
        self.custom_gui_fpath = "/zynthian/zynthian-ui/zyngui/zynthian_widget_librespot.py"

        # self.command = ["/root/librespot/target/debug/librespot", "--name", self.jackname ]
        
        # self.command = "/root/librespot/target/debug/librespot --name {}".format(self.jackname)
        # self.command = "/root/librespot/target/release/librespot --bitrate 320 --name '{}/{}' -f F32".format(self.jackname, socket.getfqdn())
        self.command = "/root/spotifyd --no-daemon -b rodiojack --audio-format f32 --bitrate 320 --use-mpris=true --no-audio-cache=true --onevent=/bin/true"
        self.command = ["/root/spotifyd", "--no-daemon",
                        "-b", "rodiojack",
                        "--audio-format", "f32",
                        "--bitrate", "320",
                        "--use-mpris=true",
                        "--no-audio-cache=true",
                        "--onevent=/bin/true"]
       
        # MIDI Controllers
        self._ctrls = [
            ['volume', None, 80, 100],
            ['position', {'value': 0.0, 'value_max': 1.0, 'is_integer': False, 'is_logarithmic': False}],
            ['prev/next', None, '<>', ['<', '<>', '>']],
            ['pause', None, 'playing', ['paused', 'playing']],
            ['shuffle', None, 'off', ['off', 'on']],
            ['seek', None, '<>', ['<', '<>', '>']],
            ['repeat', None, 'none', ['none', 'track', 'all']]
        ]

        # Controller Screens
        self._ctrl_screens = [
            ['main', ['position', 'repeat', 'prev/next', 'pause']],
            ['settings', ['volume', 'shuffle', 'seek']]
        ]
    
        self.session_bus = dbus.SessionBus()
        self.start()

    # ---------------------------------------------------------------------------
    # Subproccess Management & IPC
    # ---------------------------------------------------------------------------

    def add_processor(self, processor):
        return super().add_processor(processor)

    def start(self):
        # super().start()
        self.proc = Popen(self.command, env=self.command_env, cwd=self.command_cwd, shell=False,
                          text=True, bufsize=1, stdout=PIPE, stderr=STDOUT, stdin=PIPE)
        sleep(1)
        # zynautoconnect.request_audio_connect()
        self.delayed_connect_outputs()
        
        # sleep(2)
        # self.start_monitor_thread()
        self.start_proc_poll_thread()
        
    
    def stop(self):
        if self.proc:
            try:
                logging.info("Stoping Engine " + self.name)
                self.proc.terminate()
                try:
                    self.proc.wait(0.2)
                except:
                    self.proc.kill()
                self.proc = None
            except Exception as err:
                logging.error(f"Can't stop engine {self.name} => {err}")
        
    def start_monitoring(self):
        """Run the D-Bus main loop in a separate thread."""
        loop = dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
        print("Starting D-Bus main loop for Spotify monitoring...")
        self.session_bus = dbus.SessionBus()
        # Get the Spotify player object
        self.spotify = self.session_bus.get_object('rs.spotifyd.instance{}'.format(self.proc.pid), '/org/mpris/MediaPlayer2')
        self.spotify_interface = dbus.Interface(self.spotify, 'org.mpris.MediaPlayer2.Player')

        # Connect to signals
        GLib.MainLoop().run()
        
        self.spotify.connect_to_signal("PropertiesChanged",
                                        self.on_properties_changed,
                                        mainloop=loop,
                                        dbus_interface="org.freedesktop.DBus.Properties")
       

    def on_properties_changed(self, interface_name, changed_properties, invalidated_properties):
        """Handle the PropertiesChanged signal."""
        if interface_name == "org.mpris.MediaPlayer2.Player":
            if 'PlaybackStatus' in changed_properties:
                print(f"Playback status changed: {changed_properties['PlaybackStatus']}")
            if 'Metadata' in changed_properties:
                print(f"Metadata changed: {changed_properties['Metadata']}")

    def start_proc_poll_thread(self):
        self.proc_poll_thread = Thread(target=self.proc_poll_thread_task, args=())
        self.proc_poll_thread.name = f"proc_poll_{self.jackname}"
        self.proc_poll_thread.daemon = True  # thread dies with the program
        self.proc_poll_thread.start()

    def start_position_thread(self):
        if (self.position_thread is None):
            self.position_thread = Thread(target=self.update_position, args=())
            self.position_thread.daemon = True  # thread dies with the program
            self.position_thread.start()        

    def start_monitor_thread(self):
        # Start the D-Bus monitoring in a separate thread
        dbus_thread = Thread(target=self.start_monitoring)
        dbus_thread.daemon = True  # Allow the thread to exit when the main program exits
        dbus_thread.start()

    def proc_cmd(self, cmd):
        x = self.proc
        if self.client:
            self.client.send(f"{cmd}\n".encode())

    def proc_poll_thread_task(self):
        last_status = 0
        last_info = 0
        line = ""
        while self.proc.poll() is None:
            now = monotonic()
            while True:
                line = self.proc.stdout.readline()
                if not line:  
                    break
                #the real code does filtering here
                if line.startswith(MAGIC_STRING):
                    try:
                        changes = json.loads(line.rstrip()[MAGIC_STRING_LEN:])
                        print(changes)
                        self.on_change(changes)
                    except:
                        pass
    
    def set_position(self, ms: int):
        length = self.monitors_dict['length']
        
        pos = ms / length
        self.monitors_dict['position'] = ms
        try:
            prcs = self.processors
            zctrl = prcs[0].controllers_dict['position']
            zctrl.set_value(pos, False)
        except Exception as e:
            print('nou moe', e)

    def on_change(self, changes):  
        if 'POSITION_MS' in changes:
            self.set_position(int(changes.get('POSITION_MS')))
            # length = self.monitors_dict['length']
            # print('Length: ', length)
            # posms = int(changes.get('POSITION_MS'))
            # print('posms', posms)
            # pos = int(changes.get('POSITION_MS')) / self.monitors_dict['length']
            # print('New pos:', pos)
            # self.monitors_dict['position'] = posms
            # try:
            #     prcs = self.processors
            #     d = prcs[0].controllers_dict
            #     prcs[0].controllers_dict['position'].set_value(pos, False)
            # except Exception as e:
            #     print('nou moe', e)
        # if 'TRACK_NAME' in changes:
        #     self.monitors_dict["title"] = changes.get('TRACK_NAME')
        #     self.getmetadata()
        if 'TRACK_COVER' in changes:
            self.monitors_dict["artwork"] = changes.get('TRACK_COVER')
        if 'PLAYER_EVENT' in changes:
            self.handle_player_event(changes)

    def reset_monitors(self, reset_title=False):
        for key in self.monitors_dict:
            if reset_title or key != "title":
                self.monitors_dict[key] = ""

    # ---------------------------------------------------------------------------
    # Processor Management
    # ---------------------------------------------------------------------------

    # ---------------------------------------------------------------------------
    # MIDI Channel Management
    # ---------------------------------------------------------------------------

    # ---------------------------------------------------------------------------
    # Bank Management
    # ---------------------------------------------------------------------------

    def get_bank_list(self, processor=None):
        try:
            with open(self.my_data_dir + "/presets/inet_radio/presets.json", "r") as f:
                self.presets = json.load(f)
        except:
            # Preset file missing or corrupt
            self.presets = self.default_presets
            """
            # Write default preset file
            json_obj = json.dumps(self.presets, indent=4)
            with open(self.my_data_dir + "/presets/inet_radio/presets.json", "w") as f:
                f.write(json_obj)
            """

        self.banks = []
        self.preset2bank = []
        playlists = None
        for bank_i, bank in enumerate(self.presets):
            self.banks.append([bank, None, bank, None])
            if bank == "Playlists":
                self.presets[bank] = [] # Clear playlists
                playlists = bank_i
                continue
            for preset_i, preset in enumerate(self.presets[bank]):
                self.preset2bank.append((bank_i, preset_i, preset[2]))

        # Append playlists
        if playlists is None:
            self.banks.append(["Playlists", None, "Playlists", None])
            self.presets["Playlists"] = []
            playlists = len(self.banks) - 1
        # Populate playlists
        for file in listdir(f"{self.my_data_dir}/capture"):
            if file[-4:].lower() in (".m3u", ".pls"):
                self.presets["Playlists"].append([f"{self.my_data_dir}/capture/{file}", 1, file[:-4]])

        return self.banks

    # ---------------------------------------------------------------------------
    # Preset Management
    # ---------------------------------------------------------------------------

    def get_preset_list(self, bank, processor=None):
        return []
        presets = []
        for preset in self.presets[bank[0]]:
            presets.append(preset)
        return copy.deepcopy(presets)

    def set_preset(self, processor, preset, preload=False):
        if preload or self.preset == preset:
            return
        self.preset = preset
        for self.preset_i, config in enumerate(self.preset2bank):
            if config[0] == processor.bank_index and config[1] == processor.preset_index:
                break
        self.pending_preset_i = self.preset_i
        self.proc_cmd("clear")
        self.proc_cmd(f"add {preset[0]}")
        self.monitors_dict['title'] = preset[2]
        if preset[1]:
            self._ctrl_screens = [
                ['main', ['volume', 'stream', 'prev/next', 'pause']],
                ['playlist', ['shuffle']]
            ]
        else:
            self._ctrl_screens = [['main', ['volume', 'stream', 'prev/next']]]
        processor.refresh_controllers()
        self.reset_monitors()
        self.delayed_connect_outputs()
        return True

    def delayed_connect_outputs(self):
        """ Trigger background delayed audio autoconnect, incase other mechanisms fail"""
        sleep(0.2)
        zynautoconnect.request_audio_connect(True)
        if self.connect_timer:
            self.connect_timer.cancel()
        self.connect_timer = Timer(2, zynautoconnect.audio_autoconnect)
        self.connect_timer.start()
        jclient = jack.Client("temp")
        try:
            jclient.disconnect(self.jackname, 'system')
        except Exception as err:
            logging.info("Error disconnecing {} from {} {}".format(self.jackname, 'system', err))
            pass
    # ----------------------------------------------------------------------------
    # Controllers Management
    # ----------------------------------------------------------------------------

    def getmetadata(self):

        try:
            spotify = self.session_bus.get_object('rs.spotifyd.instance{}'.format(self.proc.pid), '/org/mpris/MediaPlayer2')
            spotify_interface = dbus.Interface(spotify, 'org.freedesktop.DBus.Properties')

            props = spotify_interface.Get('org.mpris.MediaPlayer2.Player', 'Metadata')

            # print(props)
            # Accessing keys and values
            for key in props.keys():
                # Convert dbus.String key to a regular string
                normal_key = str(key)
                # Convert dbus.String value to a regular string
                normal_value = str(props[key])
                if normal_key == 'xesam:title':
                    self.monitors_dict['title'] = normal_value
                if normal_key == 'mpris:trackid':
                    self.track_id = props[key]
                if normal_key == 'mpris:length':
                    normal_value = int(props[key])
                    try:
                        self.monitors_dict['length'] = normal_value // 1000
                    except Exception as e:  
                        print('Gekke geit', e)
                
                
                print(f"{normal_key}: {normal_value}") 

            kArtist = dbus.String('xesam:artist')
            artists = props.get(kArtist)
            artist = ''
            # Iterating over the dbus.Array
            for item in artists:
                # Convert dbus.String to a regular string
                artist = artist + '/' + str(item)
            artist = artist[1:]
            self.monitors_dict['artist'] = artist
            print(f"Artist::: {artist}")
        except dbus.DBusException as e:
            logging.debug(f"An error occurred: {e}")

    def update_position(self):
        while True:
            sleep(1)
            try:
                d = self.processors[0].controllers_dict
                if (d['pause'].get_value() == 0):
                    pass
                else:
                    pos = self.monitors_dict['position'] or 0
                    self.set_position(pos + 1000)
            except:
                pass

    def handle_player_event(self, changes):
        event = changes.get('PLAYER_EVENT')
        if (event == 'sessiondisconnected'):
            self.monitors_dict['connected'] = False
            self.monitors_dict['title'] = 'disconnected'
            self.monitors_dict['artwork'] = ''
        if (event == 'sessionconnected'):
            self.monitors_dict['connected'] = True
            self.start_position_thread()
        if (event == 'change'):
            self.getmetadata()
        if (event == 'start'):
            self.processors[0].controllers_dict['pause'].nudge(1)
        if (event == 'pause'):
            self.processors[0].controllers_dict['pause'].nudge(-1)
        if (event == 'shuffle_changed'):
            self.processors[0].controllers_dict['shuffle'].nudge(1 if changes.get('SHUFFLE') == 'true' else -1, False)
        if (event == 'repeat_changed'):
            self.processors[0].controllers_dict['repeat'].set_value(changes.get('REPEAT'), False)

    def play(self):
        
        try:
            spotify = self.session_bus.get_object('rs.spotifyd.instance{}'.format(self.proc.pid), '/org/mpris/MediaPlayer2')
            spotify_interface = dbus.Interface(spotify, 'org.mpris.MediaPlayer2.Player')

            spotify_interface.Play()
            print("Playing")
        except dbus.DBusException as e:
            logging.debug(f"An error occurred: {e}")

    def pause(self):
        
        try:
            spotify = self.session_bus.get_object('rs.spotifyd.instance{}'.format(self.proc.pid), '/org/mpris/MediaPlayer2')
            spotify_interface = dbus.Interface(spotify, 'org.mpris.MediaPlayer2.Player')

            spotify_interface.Pause()
            print("Paused")
        except dbus.DBusException as e:
            logging.debug(f"An error occurred: {e}")

    def seek(self, s):
        
        try:
            spotify = self.session_bus.get_object('rs.spotifyd.instance{}'.format(self.proc.pid), '/org/mpris/MediaPlayer2')
            spotify_interface = dbus.Interface(spotify, 'org.mpris.MediaPlayer2.Player')

            spotify_interface.Seek(s * 1000)
            print(f"Seeked {'+' if s > 0 else '-'}{s} seconds")
        except dbus.DBusException as e:
            logging.debug(f"An error occurred: {e}")

    @debounce(0.1)
    def send_position(self, s) -> None:  
        try:
            if self.track_id is None:
                return
            spotify = self.session_bus.get_object('rs.spotifyd.instance{}'.format(self.proc.pid), '/org/mpris/MediaPlayer2')
            spotify_interface = dbus.Interface(spotify, 'org.mpris.MediaPlayer2.Player')
            print(f"Setting {self.track_id} to {s // 1000} seconds")
            spotify_interface.SetPosition(self.track_id, s * 1000)
        except dbus.DBusException as e:
            logging.debug(f"An error occurred: {e}")


    def next_song(self):
        
        try:
            # Get the Spotify interface
            spotify = self.session_bus.get_object('rs.spotifyd.instance{}'.format(self.proc.pid), '/org/mpris/MediaPlayer2')
            spotify_interface = dbus.Interface(spotify, 'org.mpris.MediaPlayer2.Player')

            # Call the Next method
            spotify_interface.Next()
            print("Skipped to the next song.")
        except dbus.DBusException as e:
            logging.debug(f"An error occurred: {e}")

    def previous_song(self):
        
        try:
            # Get the Spotify interface
            spotify = self.session_bus.get_object('rs.spotifyd.instance{}'.format(self.proc.pid), '/org/mpris/MediaPlayer2')
            spotify_interface = dbus.Interface(spotify, 'org.mpris.MediaPlayer2.Player')

            # Call the Next method
            spotify_interface.Previous()
            print("Skipped to the previous song.")
        except dbus.DBusException as e:
            logging.debug(f"An error occurred: {e}")

    def shuffle(self, state):
        
        try:
            # Get the Spotify interface
            spotify = self.session_bus.get_object('rs.spotifyd.instance{}'.format(self.proc.pid), '/org/mpris/MediaPlayer2')
            spotify_interface = dbus.Interface(spotify, 'org.freedesktop.DBus.Properties')
            spotify_interface.Set('org.mpris.MediaPlayer2.Player', 'Shuffle', dbus.Boolean(state))
            print("Changed shuffle")
        except dbus.DBusException as e:
            logging.debug(f"An error occurred: {e}")

    def repeat(self, zctrl):
        
        try:
            # Get the Spotify interface
            spotify = self.session_bus.get_object('rs.spotifyd.instance{}'.format(self.proc.pid), '/org/mpris/MediaPlayer2')
            spotify_interface = dbus.Interface(spotify, 'org.freedesktop.DBus.Properties')
            spdict = {0: 'None', 1: 'Track', 2: 'Playlist'}
            sval = spdict[zctrl.value]
            spotify_interface.Set('org.mpris.MediaPlayer2.Player', 'LoopStatus', dbus.String(sval))
            print("Changed shuffle")
        except dbus.DBusException as e:
            logging.debug(f"An error occurred: {e}")


    def send_controller_value(self, zctrl):
        if self.proc is None:
            return
        if zctrl.symbol == "volume":
            self.proc_cmd(f"volume {zctrl.value * 3}")
        elif zctrl.symbol == "prev/next":
            value = zctrl.value - 1
            zctrl.set_value(1, False)
            if value > 0:
                self.next_song()
            elif value < 0:
                self.previous_song()
            self.reset_monitors(True)
            # self.proc_cmd("info")
            sleep(0.2)
            zynautoconnect.request_audio_connect(True)
            self.delayed_connect_outputs()
            return
        elif zctrl.symbol == "position":
            value = zctrl.value * self.monitors_dict['length']
            self.send_position(value)
            return
        elif zctrl.symbol == "seek":
            value = zctrl.value - 1
            zctrl.set_value(1, False)
            if value > 0:
                self.seek(+30)
            elif value < 0:
                self.seek(-30)
            self.reset_monitors(True)
            return
        elif zctrl.symbol == "pause":
            # Cannot set absolute pause mode so force pause then toggle
            if zctrl.value:
                self.play()
            else:
                self.pause()
        elif zctrl.symbol == "shuffle":
            self.shuffle(zctrl.value)
        elif zctrl.symbol == "repeat":
            self.repeat(zctrl)
        return

    def get_monitors_dict(self):
        return self.monitors_dict

    # ---------------------------------------------------------------------------
    # Specific functions
    # ---------------------------------------------------------------------------

    # ---------------------------------------------------------------------------
    # API methods
    # ---------------------------------------------------------------------------

# ******************************************************************************
