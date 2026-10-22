# gst-plugin-oamd

Works with [atmos_tools.py](https://gist.github.com/yuygfgg/8a2686b230b2919b1d59f88addd4b0f6)

## Build

```sh
cmake --preset macos-x86_64-reference-player \
  -S gst-plugin-oamd \
  -DGLAZE_SOURCE_DIR=/tmp/glaze-v7.3.2 \
  -DGST_HOME_AUDIO_SOURCE_DIR="$HOME/gst-home-audio"
cmake --build --preset macos-x86_64-reference-player
```

## Inspect

```sh
GST_PLUGIN_SCANNER="/Applications/Dolby/Dolby Reference Player.app/Contents/Frameworks/GStreamer.framework/Versions/1_16/Resources/libexec/gst-plugin-scanner" \
GST_PLUGIN_PATH="/Users/a1/gst-plugin-oamd/build/cmake-x86_64/reference-player-plugins" \
GST_PLUGIN_SYSTEM_PATH_1_0="/Applications/Dolby/Dolby Reference Player.app/Contents/Frameworks/GStreamer.framework/Versions/1_16/Resources/lib/gstreamer-1.0" \
"/Applications/Dolby/Dolby Reference Player.app/Contents/Frameworks/GStreamer.framework/Versions/1_16/Resources/bin/gst-inspect-1.0" dlboamdmod
```

## Usage

```sh
GST_PLUGIN_SCANNER="/Applications/Dolby/Dolby Reference Player.app/Contents/Frameworks/GStreamer.framework/Versions/1_16/Resources/libexec/gst-plugin-scanner" \
GST_PLUGIN_PATH="/Users/a1/gst-plugin-oamd/build/cmake-x86_64/reference-player-plugins" \
GST_PLUGIN_SYSTEM_PATH_1_0="/Applications/Dolby/Dolby Reference Player.app/Contents/Frameworks/GStreamer.framework/Versions/1_16/Resources/lib/gstreamer-1.0" \
"/Applications/Dolby/Dolby Reference Player.app/Contents/Frameworks/GStreamer.framework/Versions/1_16/Resources/bin/gst-launch-1.0" -q \
filesrc location="/path/to/file.mlp" ! \
dlbtruehdparse align-major-sync=false ! \
dlbtruehddec out-ch-config=21 presentation=16 ! \
oamdserialize ! \
filesink location=/tmp/audio.metadata
```

```sh
GST_PLUGIN_SCANNER="/Applications/Dolby/Dolby Reference Player.app/Contents/Frameworks/GStreamer.framework/Versions/1_16/Resources/libexec/gst-plugin-scanner" \
GST_PLUGIN_PATH="/Users/a1/gst-plugin-oamd/build/cmake-x86_64/reference-player-plugins" \
GST_PLUGIN_SYSTEM_PATH_1_0="/Applications/Dolby/Dolby Reference Player.app/Contents/Frameworks/GStreamer.framework/Versions/1_16/Resources/lib/gstreamer-1.0" \
"/Applications/Dolby/Dolby Reference Player.app/Contents/Frameworks/GStreamer.framework/Versions/1_16/Resources/bin/gst-launch-1.0" -q \
filesrc location="/path/to/file.mlp" ! \
dlbtruehdparse align-major-sync=false ! \
dlbtruehddec out-ch-config=21 presentation=16 ! \
tee name=t \
  ! queue ! oamdcapsfeature remove=true ! audioconvert ! audio/x-raw,format=S24LE,layout=interleaved ! filesink location=/tmp/audio.pcm \
t. ! queue ! oamdserialize ! filesink location=/tmp/audio.metadata
```

```sh
GST_PLUGIN_SCANNER="/Applications/Dolby/Dolby Reference Player.app/Contents/Frameworks/GStreamer.framework/Versions/1_16/Resources/libexec/gst-plugin-scanner" \
GST_PLUGIN_PATH="/Users/a1/gst-plugin-oamd/build/cmake-x86_64/reference-player-plugins" \
GST_PLUGIN_SYSTEM_PATH_1_0="/Applications/Dolby/Dolby Reference Player.app/Contents/Frameworks/GStreamer.framework/Versions/1_16/Resources/lib/gstreamer-1.0" \
"/Applications/Dolby/Dolby Reference Player.app/Contents/Frameworks/GStreamer.framework/Versions/1_16/Resources/bin/gst-launch-1.0" -q \
filesrc location="/path/to/file.eac3" ! \
dlbac3parse enable-metadata=true ! \
dlbac3dec out-ch-config=21 ! \
oamdserialize ! \
filesink location=/tmp/audio.metadata
```

```sh
GST_PLUGIN_SCANNER="/Applications/Dolby/Dolby Reference Player.app/Contents/Frameworks/GStreamer.framework/Versions/1_16/Resources/libexec/gst-plugin-scanner" \
GST_PLUGIN_PATH="/Users/a1/gst-plugin-oamd/build/cmake-x86_64/reference-player-plugins" \
GST_PLUGIN_SYSTEM_PATH_1_0="/Applications/Dolby/Dolby Reference Player.app/Contents/Frameworks/GStreamer.framework/Versions/1_16/Resources/lib/gstreamer-1.0" \
"/Applications/Dolby/Dolby Reference Player.app/Contents/Frameworks/GStreamer.framework/Versions/1_16/Resources/bin/gst-launch-1.0" -q \
filesrc location="/path/to/file.eac3" ! \
dlbac3parse enable-metadata=true ! \
dlbac3dec out-ch-config=21 ! \
tee name=t \
  ! queue ! oamdcapsfeature remove=true ! audioconvert ! audio/x-raw,format=S24LE,layout=interleaved ! filesink location=/tmp/audio.pcm \
t. ! queue ! oamdserialize ! filesink location=/tmp/audio.metadata
```

## atmos_tools.py

```sh
python3 atmos_tools.py -i "/path/to/file" --mode decode -o /tmp/output
python3 atmos_tools.py -i "/path/to/file" --mode binaural -o /tmp/output.wav
```
