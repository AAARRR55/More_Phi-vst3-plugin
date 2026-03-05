# Dataset Generation Guide for MorphSnap

MorphSnap now includes a built-in dataset generator for creating training data for Machine Learning models. This tool renders audio clips with various parameter configurations from a hosted plugin and exports both the audio and the corresponding parameter metadata.

## How to use via MCP

You can trigger dataset generation using the `generate_dataset` tool provided by the embedded MCP server.

### Example Request

```json
{
  "method": "generate_dataset",
  "params": {
    "samples": 250,
    "duration": 2.0,
    "input_audio": "C:/Audio/Drums_Loop.wav",
    "output_path": "C:/ML_Datasets/Drums_Processor",
    "respect_sanity": true
  }
}
```

### Parameters

- `samples` (integer): Number of random parameter states to generate (default: 100).
- `duration` (number): Duration in seconds of audio to render per state (default: 1.0).
- `input_audio` (string): Absolute path to a source audio file to process through the plugin. If omitted, the plugin processes silence.
- `output_path` (string): Absolute path for the output directory. If omitted, uses a timestamped folder in Documents/MorphSnap_Datasets.
- `respect_sanity` (boolean): Whether to avoid randomizing dangerous parameters (Volume, Pitch, Bypass) based on the current SanityConfig (default: true).

## Output Structure

The tool generates the following files in the specified `output_path`:

1.  `dataset_audio.wav`: A single continuous WAV file containing all rendered clips concatenated.
2.  `dataset_metadata.json`: A JSON array where each entry corresponds to a clip in the WAV file.

### Metadata Schema

```json
[
  {
    "id": 0,
    "parameters": [0.12, 0.45, 0.89, ...],
    "timestamp": 1740984000000
  },
  ...
]
```

The `parameters` array contains normalized float values (0.0 to 1.0) for every parameter in the hosted plugin, in the same order as returned by `list_parameters`.

## Architecture Details

- **Headless Rendering**: Uses `DatasetGenerator.cpp` to process the audio block-by-block without using the hardware audio device.
- **Parametric Consistency**: Captures the exact floating-point values applied to the plugin before processing each block.
- **Safety**: Integrates with MorphSnap's `SanityConfig` to prevent speaker-damaging volume spikes or CPU-intensive parameter changes during large batch generation.
