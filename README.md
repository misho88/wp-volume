# wp-volume

Simple WirePlumber volume control intended for use in scripts. It changes the playback/capture volume/mute state then responds with a description of the new state.

# why?

PipeWire's ALSA interface doesn't allow volume to go over 100%. WirePlumber doesn't seem to complain.

# usage

Should be exactly like: https://github.com/misho88/alsa-volume

Setting a volume over 100% will work, though.

# references

I used the source code for `pwctl` to figure out how to interface with WirePlumber: https://gitlab.freedesktop.org/pipewire/wireplumber/-/blob/master/src/tools/wpctl.c

It is listed as Copyright © 2019-2020 Collabora Ltd. George Kiagiadakis <george.kiagiadakis@collabora.com>

# requirements

PipeWire with WirePlumber
