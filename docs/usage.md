# Using Halftape

Play audio in the source app, choose it in **Capture**, then click **Start Capture**. Allow system audio recording when macOS asks. Halftape keeps a rolling buffer of the last three minutes. Click **Grab last 30s** to make a practice clip; the button shows the available time if there is less. **Stop Capture** keeps the buffer, while a new capture replaces it. **Demo** plays a sample clip without recording.

You can pause the source app before playing a clip, or turn on **Pause source on Grab**. Control–Option–G grabs from an available buffer. It does not start capture.

In Practice, use **Play** and **Loop** to play a clip. Drag A and B on the timeline to set the loop. Click or drag the timeline to seek. Use the stem controls to solo or mute separated parts; unfinished regions play the original mix. The speed and pitch controls support slower practice and transposition.

## Live Sing

Live Sing removes vocals from audio as it plays in the selected source app. Start capture first, then choose a mode and click **Start Sing**. The mode picker is available while Sing is stopped.

- **Clean** uses UMX-L with a longer buffer for vocal separation. The panel shows the measured delay once Sing is playing. Run `scripts/sing-clean-model.sh` to prepare the model, or choose an existing `umxl-vocals.onnx` in the app.
- **Low latency** uses the StemGenRT hop-128 model. Run `scripts/sing-model.sh` to download it, or choose an existing `stemgenrt-hop128.onnx`.

Keep the source playing during Sing: Halftape mutes its original output and plays the processed audio. Skip, go back or pause from the menu bar, with Control–Option–P, Control–Option–Space and Control–Option–N, or in the source app. Spotify and Music are controlled directly; other apps get the media keys, which needs Accessibility access. **Stop Sing** restores the original audio and stops capture; **Return to Clip** also resumes your practice clip.

### Lyrics mirror

Because Sing plays the audio a little late, lyrics in the source app run ahead of what you hear. The **Lyrics mirror** in the Sing panel covers the lyrics area of the source window with a copy of itself delayed by the same amount, so the lyrics and the audio line up again. Everything else in the window stays live.

Turn it on, click **Set lyrics area…** and drag a box over the lyrics. The box is saved for that app and window size, and **Show area** outlines it whenever you want to check it. If you resize the window, set it again. macOS asks for Screen Recording access the first time; allow Halftape, then relaunch it, since macOS applies that permission at launch. On macOS 15 the system also asks again now and then, which is how it treats every app that captures the screen.

The status line in the panel says what the mirror is doing. While it is on screen, a small badge in its corner shows how far behind the lyrics are, and the area flashes an orange outline when the mirror takes over. The badge can be turned off. The mirror only shows while Sing is playing and the source window is on screen. It hides while the pointer moves inside the source window, while you type in it, and whenever another window covers the lyrics, so you can change songs without closing anything. A track change clears the delayed frames so the new song starts clean.

Vocals can remain even at 100% reduction. Transpose adds delay. The delay shown includes the audio device as macOS reports it. With Low Power Mode on, Clean Sing may fall behind and stutter on some Macs. The Sing panel warns when it is on. If that happens, turn it off in System Settings under Battery, or use Low latency, which needs far less compute.

## Models and saved clips

Practice stem separation uses HTDemucs models. Run `scripts/models.sh` to download the available models. Halftape also lets you choose a compatible model file in the app. Model files are kept outside the app bundle; see [Third-party notices](../THIRD_PARTY.md) for component and model provenance.

Halftape keeps the last 20 grabbed clips for the current session. Clips and stems are temporary and are not saved across app launches; there is no export action.
