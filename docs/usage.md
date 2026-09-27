# Using Halftape

Play audio in another app, then choose it as the source in Halftape. Selecting a source starts capture. Allow system audio recording when macOS asks. The buffer keeps the last three minutes; **Grab** turns the selected duration into a practice clip. **Disarm source** stops capture and clears the buffer.

Pause the source before playing a clip, or enable **Pause source on Grab**. Control–Option–G grabs recent audio.

In Practice, drag A and B to set the loop. Space plays or pauses the clip. Use the stem controls to solo or mute separated parts; unfinished regions play the original mix. The speed and pitch controls support slower practice and transposition.

## Live Sing

Control–Option–K starts Sing for the selected source. Keep the source playing: Halftape mutes its original output and plays delayed processed audio. Sing buffers six seconds of audio before processing.

Sing can switch from six-stem separation to four stems or center cancellation when processing falls behind. Center cancellation can also remove centered instruments and leave vocals audible. **Stop Sing** restores the original audio.

## Models and saved clips

Practice stem separation uses HTDemucs models. Run `scripts/models.sh` to download the available models. Halftape also lets you choose a compatible model file in the app. Model files are kept outside the app bundle; see [Third-party notices](../THIRD_PARTY.md) for component and model provenance.

Halftape keeps the last 20 grabbed clips for the current session. Clips and stems are temporary and are not saved across app launches; there is no export action.
