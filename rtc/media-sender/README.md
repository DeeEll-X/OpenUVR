# Example streaming to browser

This is a copy/paste example forwarding a local RTP stream to the browser.

## How to use

Open main.html in your browser.

```
gst-launch-1.0 -v filesrc location = videotest.mp4 ! qtdemux ! video/x-h264! rtph264pay pt=96 mtu=1200 ! udpsink host=127.0.0.1 port=6000
```
