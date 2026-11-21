#ifndef UG2_DEFAULT_CFG_H_
#define UG2_DEFAULT_CFG_H_

// Text of the settings file written on the first start; every line is a comment, so nothing changes until edited.

namespace ug2 {

const char DefaultSettingsFile[] =
    "# NFS Underground 2 for the PS Vita: settings.\n"
    "# Each setting below is shown at its default. To change one, remove the # at the start of its line and\n"
    "# edit the value, then start the game again. Delete this file to get a fresh copy. Pressing START on the\n"
    "# start-up screen opens a menu that edits this file for you.\n"
    "\n"
    "# The start-up screen with its 3 second countdown (START configures, any other button skips): on or off.\n"
    "#show_configurator_on_boot=on\n"
    "\n"
    "# Frame rate: 30 locks to 30 frames a second for steady pacing, 0 unlocks it.\n"
    "#frame_rate=30\n"
    "\n"
    "# Picture: on fills the 16:9 screen with the HUD at the edges, off is the original 4:3 with black bars.\n"
    "#widescreen=on\n"
    "\n"
    "# Graphics preset: vita is what the port is tuned for, max turns everything on and is much slower.\n"
    "# The game's own Display menu changes the individual options; above Medium detail the Vita struggles.\n"
    "#gfx_preset=vita\n"
    "\n"
    "# Rear touch pad for shifting and the camera: on or off.\n"
    "#rear_touch=on\n"
    "\n"
    "# Front touch screen as a mouse click in the menus (no cursor is drawn): on or off.\n"
    "#front_touch=on\n"
    "\n"
    "# Right stick turns the car in the garage and the customisation menus: on or off. The dead zone is a\n"
    "# percentage of the stick's range.\n"
    "#garage_stick=on\n"
    "#garage_stick_deadzone=30\n"
    "\n"
    "# Scenery narrower than this many pixels on screen uses its simpler model. 0 is off; 128 gives more\n"
    "# speed, and higher values show more pop-in.\n"
    "#dev_world_lod_pixels=0\n"
    "\n"
    "# Scenery smaller than this many pixels on screen is not drawn. 0 is off, 64 the most. More speed,\n"
    "# more pop-in.\n"
    "#dev_world_min_pixels=0\n"
    "\n"
    "# on skips the menus and starts a test race.\n"
    "#boot_race=off\n"
    "\n"
    "# Cheaper tyre smoke and other particles, close to the original picture: on or off (off is as the original).\n"
    "#optimised_particles=on\n"
    "\n"
    "# Music decoded by the Vita's hardware MP3 decoder instead of the game's own decoder, which frees the CPU:\n"
    "# on or off. It sounds the same; if the hardware decoder cannot start, the game's decoder plays it.\n"
    "#music_hw_decode=on\n"
    "\n"
    "# The sound mixer holds the game's sound lock for a shorter time, so the game waits on it less: on or off.\n"
    "#audio_mixer_split=on\n"
    "\n"
    "# Sounds quieter than this many dB below full scale are not mixed, which saves processor time; off mixes\n"
    "# every sound. -60 is inaudible; louder values such as -40 save more.\n"
    "#audio_silence_db=-60\n"
    "\n"
    "# on writes a detailed log to NeedForSpeedUnderground2.log, useful when reporting a problem. Errors\n"
    "# are logged either way.\n"
    "#log_info=off\n";

}

#endif /* !UG2_DEFAULT_CFG_H_ */
