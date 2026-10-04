# Using Halftape

Play audio in the source app, choose it in **Capture**, then click **Start Capture**. Allow system audio recording when macOS asks. Halftape keeps a rolling buffer of the last three minutes. Click **Grab last 30s** to make a practice clip; the button shows the available time if there is less. **Stop Capture** keeps the buffer, while a new capture replaces it. **Demo** plays a sample clip without recording.

You can pause the source app before playing a clip, or turn on **Pause source on Grab**. Control–Option–G grabs from an available buffer. It does not start capture.

In Practice, use **Play** and **Loop** to play a clip. Drag A and B on the timeline to set the loop. Click or drag the timeline to seek. Use the stem controls to solo or mute separated parts; unfinished regions play the original mix. The speed and pitch controls support slower practice and transposition.

## Live Sing

Start capture, open **Live Sing**, then click **Start Sing**. Sing uses StemGenRT for live vocal separation. The model is available through `scripts/sing-model.sh`, or **Choose Sing Model** can open an existing `stemgenrt-hop128.onnx`.

Keep the source playing: Halftape mutes its original output and plays the processed audio. Pause and seek in the source app. **Stop Sing** restores the original audio and stops capture. **Return to Clip** also resumes your practice clip.

Vocals can remain even at 100% reduction. Transpose adds delay, and the processing estimate excludes device and Bluetooth latency. Low Power Mode can make Sing fall behind.

## Models and saved clips

Practice stem separation uses HTDemucs models. Run `scripts/models.sh` to download the available models. Halftape also lets you choose a compatible model file in the app. Model files are kept outside the app bundle; see [Third-party notices](../THIRD_PARTY.md) for component and model provenance.

Halftape keeps the last 20 grabbed clips for the current session. Clips and stems are temporary and are not saved across app launches; there is no export action.
