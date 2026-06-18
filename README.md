# Sayo_Streaming
Streaming graphics to SayoDevice O3C for linux distros

# Dependencies
- libusb-1.0
- ffmpeg

# Install
[Releases](https://github.com/binik17/Sayo_Streaming/releases/tag/1.0)

Or compile
```
git clone https://github.com/binik17/Sayo_Streaming && cd Sayo_Streaming
gcc sayo_stream.c -o sayo_streaming -lusb-1.0 -O3
```

# Usage
Sayo_streaming uses /dev/video50 as a source. You can create a virtual camera using v4l2loopback and OBS
```
sudo modprobe v4l2loopback video_nr=50 card_label="OBS Virtual Camera" exclusive_caps=1
# Simply start OBS vurtual camera and run sayo_streaming
```
