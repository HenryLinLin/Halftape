# Using Halftape

Play audio in the source app, choose it in **Capture**, then click **Start Capture**. Allow system audio recording when macOS asks. Halftape keeps a rolling buffer of the last three minutes. Click **Grab last 30s** to make a practice clip; the button shows the available time if there is less. **Stop Capture** keeps the buffer, while a new capture replaces it. **Demo** plays a sample clip without recording.

You can pause the source app before playing a clip, or turn on **Pause source on Grab**. Control–Option–G grabs from an available buffer. It does not start capture.

In Practice, drag A and B to set the loop. Space plays or pauses the clip. Use the stem controls to solo or mute separated parts; unfinished regions play the original mix. The speed and pitch controls support slower practice and transposition.

## Live Sing

Start capture, then press Control–Option–K to start Sing. Keep the source playing: Halftape mutes its original output and plays delayed processed audio. Sing buffers six seconds of audio before processing.

Sing can switch from six-stem separation to four stems or center cancellation when processing falls behind. Center cancellation can also remove centered instruments and leave vocals audible. **Stop Sing** restores the original audio.

## Models and saved clips

Practice stem separation uses HTDemucs models. Run `scripts/models.sh` to download the available models. Halftape also lets you choose a compatible model file in the app. Model files are kept outside the app bundle; see [Third-party notices](../THIRD_PARTY.md) for component and model provenance.

Halftape keeps the last 20 grabbed clips for the current session. Clips and stems are temporary and are not saved across app launches; there is no export action.
