/*
 * More-Phi - AI/Dataset/MetadataSchema.cpp
 * JSON schema initialization for MetadataWriter.
 */
#include "MetadataWriter.h"

namespace more_phi {

void MetadataWriter::initializeSchema()
{
    schema_ = nlohmann::json::parse(R"json(
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "title": "MorePhi Dataset Metadata",
  "type": "object",
  "required": ["sampleId", "timestamp", "source", "chain", "output"],
  "additionalProperties": true,
  "properties": {
    "sampleId": {"type": "string"},
    "timestamp": {"type": "integer"},
    "source": {
      "type": "object",
      "required": ["filePath"],
      "additionalProperties": true,
      "properties": {
        "filePath": {"type": "string"},
        "genre": {"type": "string"},
        "contentType": {"type": "string"},
        "originalLufs": {"type": "number"},
        "dynamicRangeDb": {"type": "number"},
        "sampleRate": {"type": "number", "exclusiveMinimum": 0},
        "numChannels": {"type": "integer", "minimum": 1},
        "numSamples": {"type": "integer", "minimum": 0},
        "fileHash": {"type": "string"}
      }
    },
    "chain": {
      "type": "object",
      "required": ["plugins"],
      "additionalProperties": true,
      "properties": {
        "chainType": {"type": "string"},
        "plugins": {
          "type": "array",
          "items": {
            "type": "object",
            "additionalProperties": true,
            "properties": {
              "pluginId": {"type": "string"},
              "pluginName": {"type": "string"},
              "vendor": {"type": "string"},
              "version": {"type": "string"},
              "format": {"type": "string"},
              "parameters": {
                "type": "array",
                "items": {
                  "type": "object",
                  "additionalProperties": true,
                  "properties": {
                    "name": {"type": "string"},
                    "index": {"type": "integer"},
                    "normalizedValue": {"type": "number", "minimum": 0, "maximum": 1},
                    "rawValue": {"type": "number"},
                    "textValue": {"type": "string"},
                    "category": {"type": "string"}
                  }
                }
              }
            }
          }
        },
        "sampleRate": {"type": "number", "exclusiveMinimum": 0},
        "blockSize": {"type": "integer", "minimum": 1}
      }
    },
    "output": {
      "type": "object",
      "additionalProperties": true,
      "properties": {
        "lufs": {"type": "number"},
        "truePeakDb": {"type": "number"},
        "dynamicRangeDb": {"type": "number"},
        "spectralCentroidHz": {"type": "number"},
        "numSamples": {"type": "integer", "minimum": 0},
        "durationSeconds": {"type": "number", "minimum": 0}
      }
    },
    "spectralFeatures": {"type": "object"},
    "temporalFeatures": {"type": "object"},
    "perceptualFeatures": {"type": "object"},
    "targets": {
      "type": "object",
      "additionalProperties": true,
      "properties": {
        "parameterRegression": {
          "type": "array",
          "items": {"type": "number", "minimum": 0, "maximum": 1}
        },
        "styleClassification": {"type": "string"},
        "processingIntensity": {"type": "number", "minimum": 0, "maximum": 1},
        "featureVector": {
          "type": "array",
          "items": {"type": "number"}
        }
      }
    },
    "split": {"type": "string", "enum": ["train", "val", "test"]},
    "tags": {
      "type": "array",
      "items": {"type": "string"}
    }
  }
}
)json");
}

} // namespace more_phi
