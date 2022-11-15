# Example streaming to browser

This is a copy/paste example forwarding a local RTP stream to the browser.

## How to use

Open main.html in your browser.

```
gst-launch-1.0 -v filesrc location = videotest.mp4 ! qtdemux ! video/x-h264! rtph264pay pt=96 mtu=1200 ! udpsink host=127.0.0.1 port=6000
```

```
gst-launch-1.0 -v filesrc location = videotest.mp4 ! qtdemux ! video/x-h264! qtmux ! filesink location=~/videotesth-gst-x264.mp4
```

```
gst-launch-1.0 -v filesrc location = videotest.mp4 ! qtdemux ! video/x-h264! qtmux ! udpsink host=127.0.0.1 port=6000
```

```
gst-launch-1.0 -v videotestsrc ! x264enc ! video/x-h264! rtph264pay pt=96 mtu=1200 ! udpsink host=127.0.0.1 port=6000
```

```
      "videotestsrc"
      "! video/x-raw, width=1920, height=1080, framerate=60/1, format=I420, pixel-aspect-ratio=1/1 interlace-mode=progressive" // can be ingored
      "! x264enc rc-lookahead=1 "
      "! video/x-h264, stream-format=byte-stream, alignment=au "
      "! rtph264pay pt=96 mtu=1200 ssrc=42 "
      "! appsink name=sink"
      );
```

```
gst-launch-1.0 -v udpsrc address=127.0.0.1 port=5000 ! video/x-h264, stream-format=byte-stream, alignment=nal ! queue ! rtph264pay pt=96 mtu=1200 ! udpsink host=127.0.0.1 port=6000
gst-launch-1.0 udpsrc address=127.0.0.1 port=6000 caps="application/x-rtp" ! queue ! rtph264depay ! video/x-h264,stream-format=byte-stream ! queue ! avdec_h264 ! queue ! autovideosink
``` 
receive from ffmpeg encoded h264 to rtp & decode rtp and h264 - get grey&green

https://ricardolu.gitbook.io/gstreamer/