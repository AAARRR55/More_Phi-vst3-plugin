# Neural Mastering Dataset Curation Report

**Feature**: `004-dataset-curation`  
**Date**: 2026-05-18  
**Target model**: Offline stereo waveform mastering model from `specs/003-neural-mastering-roadmap/neural-training-implementation-plan.md`  
**Decision posture**: Research curation only. No dataset listed here is automatically cleared for commercial model release.

## Executive Recommendation

Use a staged data mix instead of relying on a single corpus:

1. **Primary paired research set**: SonicMasterDataset. It is the closest public fit for our input/target training contract because it provides degraded/high-quality music pairs with mastering-relevant degradation groups.
2. **Multitrack synthesis set**: Cambridge-MT, MUSDB18-HQ, MedleyDB 2.0, MoisesDB, Open Multitrack Testbed, and Slakh2100. These do not provide verified mastered/unmastered labels at enough scale, but they let us synthesize aligned unmastered/mastered pairs and stress-test stem/mix diversity.
3. **Reference distribution set**: MTG-Jamendo and FMA. These are useful for genre/style/loudness distribution analysis and reference-only pretraining, not supervised mastering pair training.
4. **Perceptual validation set**: ODAQ. It is too small and not a mastering-pair corpus, but it is valuable for calibrating objective audio-quality metrics against listener scores.
5. **Hold/reject set**: Commercial stem stores, streaming-service catalogs, leaked stems, and rights-unclear uploads. These should not enter training unless rights are explicitly cleared in writing.

The best immediate research recipe is:

```text
SonicMasterDataset
+ synthetic pair generation from Cambridge-MT/MUSDB18-HQ/MedleyDB/MoisesDB
+ reference distribution checks from MTG-Jamendo/FMA
+ ODAQ metric calibration
```

Commercial readiness is blocked until all training items have item-level provenance, license status, split identity, and legal approval.

## Scoring Rubric

| Score | Meaning |
|-------|---------|
| 5 | Strong direct fit for stereo mastering research |
| 4 | Strong auxiliary fit; needs synthesis, filtering, or license gating |
| 3 | Useful for validation, reference modeling, or constrained pretraining |
| 2 | Limited use; narrow, synthetic, lossy, small, or mismatched |
| 1 | Do not use without special approval |

## Curated Dataset Matrix

| Dataset / Source | Type | Scale | Modality / Labels | License Posture | Pipeline Fit | Overfitting / Bias Risk | Recommendation |
|------------------|------|-------|-------------------|------------------|--------------|--------------------------|----------------|
| [SonicMasterDataset](https://huggingface.co/datasets/amaai-lab/SonicMasterDataset) | Paired degraded/high-quality music restoration and mastering | ~166k rows, 506 GB, 44.1 kHz examples visible | `input_flac`, `gt_flac`, degradation metadata, prompts, genre/quality metadata | HF card lists `cc-by-2.0`; source music is Jamendo-derived, so item-level provenance should still be audited | **5/5**: closest direct match for degraded-to-high-quality stereo mastering training | Synthetic degradation recipes can be memorized; all examples derive from high-quality references, not true pre-master prints | **Primary research training candidate** |
| [Cambridge-MT Mixing Secrets Library](https://www.cambridge-mt.com/ms/mtk/) | Industry-standard educational multitrack/mixing/mastering practice library | 625 projects; 16/24-bit WAV at 44.1 kHz; many mastering-tagged projects include unmastered WAVs | Raw multitracks, mixes, previews, some unmastered mix WAVs | Educational only; no commercial use without copyright-holder permission | **4/5**: excellent source for synthetic mastering pairs and small real unmastered-mix evaluation | Genre and contributor imbalance; educational license; repeated use in other datasets can cause leakage | **Research-only synthesis and evaluation candidate** |
| [SolidStateBusComp / Diff-SSL-G-Comp](https://huggingface.co/datasets/amphion/SolidStateBusComp) | Hardware compressor input/output pairs | 175 unmastered songs, 220 parameter combinations, about 2.5k hours, 4.8 TB | Normalized inputs and SSL bus-compressor ground truth outputs | `cc-by-nc-4.0`, gated access, Cambridge constraints also apply | **4/5** for dynamics module pretraining, **2/5** for full mastering | Same 175 source songs repeated across many parameter settings; strong risk of learning compressor settings instead of mastering judgment | **Auxiliary dynamics pretraining only** |
| [MUSDB18-HQ](https://sigsep.github.io/datasets/musdb.html) | Source-separation multitrack corpus | 150 full-length tracks, ~10 hours, stereo 44.1 kHz, 22.7 GB HQ WAV | Mixture plus drums, bass, vocals, other stems; train/test split | Academic access; mixed upstream licenses and per-track license list | **4/5** for stem-based synthetic pair creation; **2/5** for direct mastering | Small catalog, known errata, source overlap with Cambridge/MedleyDB must be deduplicated | **Research synthesis candidate** |
| [MedleyDB 2.0](https://medleydb.weebly.com/description.html) | Multitrack corpus with raw/stem/mix hierarchy | 196 multitracks; 44.1 kHz 16-bit WAV | Mix, processed stems, raw audio, metadata, genre, annotations | CC BY-NC-SA for non-commercial use | **4/5** for controlled multitrack synthesis; **2/5** for direct mastering labels | Mixes may or may not be mastered and are not labeled as such; small catalog | **Research synthesis and validation candidate** |
| [MoisesDB](https://github.com/moises-ai/moises-db) | Source separation beyond four stems | Public multitrack/stem dataset; 14 hours reported in secondary literature; organized by stems/sources | Hierarchical stems, mixture, genre, activity, bleed metadata | CC BY-NC-SA 4.0 | **4/5** for diverse stem mixing and source-based pair synthesis | Non-commercial/share-alike restrictions; source separation taxonomy does not equal mastering target labels | **Research synthesis candidate** |
| [Open Multitrack Testbed](https://qmro.qmul.ac.uk/xmlui/handle/123456789/22148?show=full) | Open multitrack repository with metadata | Described as large/diverse and publicly accessible | Multitrack audio, mixes/processed versions, metadata, process parameters where available | CC BY 4.0 in the cited proceeding; verify current item-level licenses | **3/5** pending access and metadata audit | Availability and current catalog status need verification; inconsistent contribution formats possible | **Conditional open multitrack candidate** |
| [Slakh2100](https://zenodo.org/records/4599666) | Synthetic multitrack/music-source corpus | 2,100 tracks, 145 hours, ~105 GB FLAC | Mono 44.1 kHz/16-bit synthetic sources, mixtures, aligned MIDI, metadata | CC BY 4.0 | **3/5** for scale, source control, and synthetic augmentation; **1/5** for direct mastering | Synthetic timbres, mono audio, known duplicate-MIDI issue, no mastering labels | **Auxiliary pretraining and augmentation candidate** |
| [MTG-Jamendo](https://github.com/MTG/mtg-jamendo-dataset) | Large reference music/tagging corpus | 55k+ full tracks with metadata and splits | Full music tracks, genre/instrument/mood tags, artist metadata | Non-commercial research; audio has individual Creative Commons licenses | **3/5** for reference distribution, genre/style balancing, and high-quality target selection | Not paired; per-track license differences; tags are uploader/community-derived | **Reference-only and selection-corpus candidate** |
| [FMA](https://github.com/mdeff/fma) | Large Creative-Commons music corpus | 106,574 tracks; 917 GiB full set; 30s and full-length subsets | Audio, genre taxonomy, metadata, features | Metadata CC BY 4.0; audio under artist-chosen licenses; research-oriented | **3/5** for broad reference distribution and pretraining; **1/5** for high-fidelity mastering pairs | MP3 encoding in common subsets, variable audio licenses, no unmastered counterpart | **Reference/pretraining candidate after license filtering** |
| [ODAQ](https://arxiv.org/abs/2401.00197) | Subjective audio-quality benchmark | 240 stereo samples in original release; 26 expert ratings per sample; expanded work reports 10,080 scores | Reference/processed audio and MUSHRA-style quality scores | Released under permissive licenses per paper; secondary sources list audio/data as CC BY 4.0 | **3/5** for metric calibration; **1/5** for mastering training | Too small and not mastering-specific; degradation classes target coding/source-separation quality | **Validation and metric-calibration candidate** |
| [AAM Artificial Audio Multitracks](https://link.springer.com/article/10.1186/s13636-023-00278-7) | Algorithmic synthetic multitrack corpus | 3,000 artificial tracks | Single-instrument tracks, mix track, precise MIDI/annotation-derived labels | Article is CC BY 4.0; dataset license/access must be verified separately before ingestion | **2/5** for synthetic pretraining and pipeline testing | Artificial composition/timbre bias; no mastering labels | **Pipeline/debugging candidate, not quality evidence** |
| Commercial stem/multitrack stores | Industry/commercial data sources | Potentially high quality, variable scale | Paid multitracks/stems, often account-bound | Usually restricted by terms; not dataset redistribution/training licenses | **1/5** unless contractually licensed | High legal risk, no reproducible public benchmark | **Reject unless custom license obtained** |
| Streaming services / public uploads / leaked stems | General music sources | Massive but uncontrolled | Mastered audio only, extracted stems if processed | Not licensed for derivative dataset creation in ordinary terms | **1/5** | Rights violations, unknown provenance, model contamination risk | **Reject** |

## Detailed Fit Notes

### 1. SonicMasterDataset

**Catalog ID**: `sonicmaster-dataset`  

This is the strongest direct match for our supervised audio-to-audio training path. It provides degraded and high-quality music pairs, sample-rate fields, degradation metadata, prompts, genre information, quality scores, and split metadata. The dataset page reports roughly 166k rows and 506 GB, with examples at 44.1 kHz. The degradation families include equalization, dynamics, amplitude, stereo, and reverb, which overlap the mastering model's expected transformations.

**Use**: primary research training and ablation baseline.  
**Do not use as sole evidence**: the degraded side is synthetic, so it does not prove performance on real pre-master prints.  
**Required controls**: hold out source IDs, group by original Jamendo track, audit source license and attribution, preserve degradation metadata, and test on real unmastered material separately.

### 2. Cambridge-MT Mixing Secrets

**Catalog ID**: `cambridge-mt`  

Cambridge-MT is highly relevant because it contains raw multitrack projects, many genre categories, and multiple projects tagged for mastering with unmastered mix WAVs. The library states that downloads are free for educational purposes and not for commercial use without copyright-holder permission.

**Use**: small real unmastered-mix validation, synthetic pair generation, and listening-test source material.  
**Do not use**: commercial model training or released training artifacts without explicit rights clearance.  
**Required controls**: project-level license records, artist/producer permission tracking, source-level split isolation, and exclusion of any overlapping projects also present in MUSDB18 or Diff-SSL-G-Comp.

### 3. SolidStateBusComp / Diff-SSL-G-Comp

**Catalog ID**: `solidstatebuscomp`  

This dataset is excellent for dynamics behavior but too narrow for full mastering. It uses 175 real unmastered Cambridge songs processed through many SSL bus-compressor parameter combinations. The Hugging Face page lists CC BY-NC 4.0, gated access, and Cambridge constraints.

**Use**: dynamics/compressor subtask pretraining and objective tests for transient/loudness behavior.  
**Do not use**: primary mastering training or commercial model release.  
**Required controls**: split by source song, not by parameter combination, or validation will leak.

### 4. MUSDB18-HQ

**Catalog ID**: `musdb18-hq`  

MUSDB18-HQ provides stereo 44.1 kHz uncompressed sources and mixtures across 150 tracks. It is small but clean and widely used. It has mixed upstream licensing and academic access requirements.

**Use**: synthetic unmastered/mastered pair generation from stems, evaluation of stem-aware rendering, and source-level data governance tests.  
**Do not use**: commercial release training until every track license is cleared.  
**Required controls**: use HQ WAV version, respect original train/test split, deduplicate with Cambridge and MedleyDB origins.

### 5. MedleyDB 2.0

**Catalog ID**: `medleydb-2`  

MedleyDB is high-quality, professionally recorded in many cases, and includes raw audio, processed stems, mix files, metadata, genre labels, and annotations. It explicitly says mixes may or may not be mastered and labels are unavailable for that distinction.

**Use**: controlled stem-to-mix synthesis, feature extractor validation, and genre/stem diversity.  
**Do not use**: as direct mastered/unmastered supervision without relabeling or synthetic target creation.  
**Required controls**: non-commercial/share-alike isolation and split by song/artist.

### 6. MoisesDB

**Catalog ID**: `moisesdb`  

MoisesDB is useful for expanding stem taxonomy beyond four stems. It provides a programmatic path to mixtures and stems plus metadata such as genre, sources, bleedings, and activity. Its license is CC BY-NC-SA 4.0.

**Use**: diverse multitrack synthesis, stem taxonomy coverage, and robustness testing.  
**Do not use**: commercial model training without rights review.  
**Required controls**: stem taxonomy normalization, bleed metadata capture, source-level split isolation.

### 7. Open Multitrack Testbed

**Catalog ID**: `open-multitrack-testbed`  

The Open Multitrack Testbed is relevant because it was designed for intelligent music production and includes multitrack audio, mixes or processed versions, metadata, and sometimes process parameters. The accessible proceeding lists CC BY 4.0.

**Use**: conditional candidate after current catalog/access verification.  
**Do not use**: as a primary source until actual downloadable items and current item-level rights are audited.  
**Required controls**: verify current access path, file consistency, process-parameter availability, and license per item.

### 8. Slakh2100

**Catalog ID**: `slakh2100`  

Slakh2100 provides scale, clean stems, and aligned MIDI under CC BY 4.0. It is synthetic and mono, so it cannot teach stereo mastering directly. It is still useful for pipeline scale tests, augmentation, and pretraining low-level spectral/dynamics reconstruction.

**Use**: synthetic pretraining, loader stress testing, source separation-aware augmentation.  
**Do not use**: as a validation claim for professional mastering.  
**Required controls**: exclude duplicate MIDI `omitted` tracks, convert mono to stereo only with explicit synthetic labels, and avoid mixing Slakh validation with real-music validation claims.

### 9. MTG-Jamendo

**Catalog ID**: `mtg-jamendo`  

MTG-Jamendo is valuable because it is large, genre-tagged, and has explicit splits. It is not paired, but it can help estimate target distribution, select high-quality references, and balance genre coverage. It is non-commercial research unless separate authorization is obtained.

**Use**: reference-only modeling, style clustering, high-quality target selection, prompt/style metadata, and data distribution baselines.  
**Do not use**: paired supervised training unless we synthesize degraded inputs and preserve per-track licenses.  
**Required controls**: item-level license capture from `audio_licenses.txt`, source-level split isolation, and no commercial use without Jamendo authorization.

### 10. FMA

**Catalog ID**: `fma`  

FMA is broad and useful for reference distribution analysis. Its common downloadable subsets are MP3-based, and audio licensing is artist-chosen, so it is weaker for high-fidelity waveform reconstruction training. The full set has scale but not mastering labels.

**Use**: genre/style distribution, broad pretraining experiments, and reference-only target statistics.  
**Do not use**: high-fidelity paired training without filtering to full-quality, license-cleared items.  
**Required controls**: license filtering, audio quality screening, codec artifact detection, and artist/album-level split isolation.

### 11. ODAQ

**Catalog ID**: `odaq`  

ODAQ is not a music mastering training set. Its value is in calibrating perceptual metrics because it pairs stereo audio examples with subjective quality scores from controlled listening tests.

**Use**: validation metric calibration, artifact sensitivity tests, and sanity checks for PEAQ/VISQOL-style measurements.  
**Do not use**: primary waveform mastering training.  
**Required controls**: keep ODAQ as held-out benchmark material only.

### 12. AAM

**Catalog ID**: `aam`  

AAM is a useful synthetic multitrack source with precise annotations and 3,000 artificial music tracks. It is not mastering-specific and should not be used to claim real-music quality.

**Use**: pipeline debugging, synthetic pretraining, and controlled augmentation tests.  
**Do not use**: product quality evidence.  
**Required controls**: verify dataset license separately from article license before ingestion.

## Recommended Initial Download Order

1. SonicMasterDataset: validate manifest fields, source IDs, split metadata, and CC attribution requirements.
2. Cambridge-MT mastering-tagged projects: manually select a small, rights-tracked research subset with unmastered mix WAVs.
3. MUSDB18-HQ and MedleyDB 2.0: ingest for controlled stem-to-mix synthesis and deduplicate against Cambridge.
4. MoisesDB: ingest after confirming license obligations and taxonomy mapping.
5. ODAQ: set aside for perceptual metric calibration, not model training.
6. MTG-Jamendo or FMA: use only after a license-filtered reference subset is defined.

## Split and Leakage Policy

All training/validation/test splits must be assigned by stable source identity:

- Original song/work ID.
- Artist and album/session ID where available.
- Dataset origin.
- Upstream catalog ID such as Jamendo ID or FMA track ID.
- Derived render family: stems, alternate mixes, degraded versions, compressor settings, mastered variants.

Never split generated variants of the same source across train and validation. This is especially important for SonicMaster degradation variants and SolidStateBusComp parameter sweeps.

## Overfitting and Bias Mitigation Table

| Risk Factor | Affected Catalog IDs | Mitigation |
|-------------|----------------------|------------|
| Duplicate sources and cross-dataset overlap | `cambridge-mt`, `musdb18-hq`, `medleydb-2`, `solidstatebuscomp` | Maintain `sourceIdentityId` and keep suspected overlaps in the same split or excluded. |
| Artist/session overlap | `cambridge-mt`, `medleydb-2`, `fma` | Split by artist and album/session, not by file or segment. |
| Synthetic degradation recipes | `sonicmaster-dataset`, `slakh2100`, `aam` | Track `derivedFamilyId`, run ablations by degradation/render family, and validate on real unmastered material. |
| Repeated parameter sweeps | `solidstatebuscomp` | Split by source song; never split compressor settings from one song across train and validation. |
| Mono-only or synthetic stereo | `slakh2100`, `aam` | Label as synthetic/mono-derived and exclude from stereo mastering quality claims. |
| Lossy encoding and codec artifacts | `fma`, some reference sources | Run codec/quality screening before waveform reconstruction pretraining. |
| Genre or catalog skew | all candidates | Track genre/source distribution and reserve held-out genre/artist groups for validation. |
| Validation-only benchmark leakage | `odaq` | Keep held-out-only and exclude from all training manifests. |

## Licensing Decision Table

| License / Rights Pattern | Training Use | Commercial Release Use | Required Action |
|--------------------------|--------------|------------------------|-----------------|
| CC BY / permissive with clear attribution | Allowed for research; likely viable for commercial if all attribution and source terms are met | Potentially viable | Capture attribution, version, source URL, checksum, and derivative notices |
| CC BY-NC or research-only | Research only | Not viable without separate permission | Keep models labeled research-only; isolate artifacts |
| CC BY-NC-SA | Research only; share-alike obligations may affect derived datasets/artifacts | Not viable without legal review and permission | Legal review before any model release |
| Educational-only | Educational/research demos only | Not viable without copyright-holder permission | Track explicit permissions per project |
| Mixed per-track licenses | Only items with captured compatible terms | Only compatible, approved items | Store item-level license metadata before ingestion |
| Unknown, leaked, streaming-derived, or account-bound content | Reject | Reject | Exclude from all datasets |

## Legal Review Follow-Up

| License / Rights Issue | Affected Catalog IDs | Current Status | Required Follow-Up |
|------------------------|----------------------|----------------|--------------------|
| Non-commercial restrictions | `solidstatebuscomp`, `medleydb-2`, `moisesdb`, `mtg-jamendo` | Research-only or commercial-blocked | Keep artifacts isolated and obtain separate commercial permission before release-intent training. |
| Educational-only restrictions | `cambridge-mt` | Research-only | Obtain project-level copyright-holder permission before any commercial model training or release. |
| Share-alike obligations | `medleydb-2`, `moisesdb` | Commercial-blocked pending legal review | Determine whether derived datasets or model artifacts carry share-alike obligations; exclude if unresolved. |
| Gated or account-bound access | `solidstatebuscomp`, `commercial-stem-stores` | Hold or research-only | Preserve access terms and do not redistribute or train release-intent models without explicit rights. |
| Mixed per-track licenses | `musdb18-hq`, `mtg-jamendo`, `fma` | Requires item-level review | Build a license ledger and filter items before any ingestion. |
| Unknown dataset rights | `aam`, `open-multitrack-testbed` | Hold pending verification | Verify current downloadable items, dataset license, and upstream provenance before use. |
| Rejected rights posture | `streaming-leaked-uploads` | Rejected | Exclude from all training, validation, benchmarking, and derived artifacts. |

## Immediate Curation Tasks

1. Build a license ledger with fields: dataset, item ID, source URL, license, attribution, commercial eligibility, share-alike/non-commercial flags, checksum, and approval status.
2. Create a source-identity registry to deduplicate Cambridge, MUSDB18, MedleyDB, and SolidStateBusComp overlap.
3. Ingest SonicMasterDataset into a research-only sandbox and verify source IDs do not cross validation boundaries.
4. Select a Cambridge-MT mastering subset for manual listening review and real pre-master validation, with clear non-commercial labeling.
5. Reserve ODAQ for metric calibration and never include it in training.
6. Produce a red/yellow/green release-eligibility report before training any model intended for product consideration.

## Rejected Sources

- Streaming-service audio and extracted stems: not licensed for derivative dataset construction under normal consumer access.
- Leaked stems or unofficial multitracks: provenance and rights are not defensible.
- Commercial stem stores without negotiated ML/data rights: ordinary access terms are not sufficient for dataset redistribution or training.
- Speech/noise-only enhancement datasets: useful for tooling tests at most, but mismatched to stereo music mastering.
- Mastered-only music catalogs without metadata or licenses: no paired supervision and high rights risk.

## Sources

- SonicMasterDataset Hugging Face dataset card: https://huggingface.co/datasets/amaai-lab/SonicMasterDataset
- SonicMaster paper: https://arxiv.org/abs/2508.03448
- Cambridge-MT Mixing Secrets library: https://www.cambridge-mt.com/ms/mtk/
- SolidStateBusComp dataset card: https://huggingface.co/datasets/amphion/SolidStateBusComp
- SolidStateBusComp project page: https://www.yichenggu.com/SolidStateBusComp/
- MUSDB18 / MUSDB18-HQ: https://sigsep.github.io/datasets/musdb.html
- MedleyDB description: https://medleydb.weebly.com/description.html
- MedleyDB home/download summary: https://medleydb.weebly.com/index.html
- MoisesDB repository: https://github.com/moises-ai/moises-db
- Slakh2100 Zenodo record: https://zenodo.org/records/4599666
- MTG-Jamendo repository: https://github.com/MTG/mtg-jamendo-dataset
- FMA dataset repository: https://github.com/mdeff/fma
- Open Multitrack Testbed proceeding record: https://qmro.qmul.ac.uk/xmlui/handle/123456789/22148?show=full
- ODAQ paper: https://arxiv.org/abs/2401.00197
- Expanded ODAQ paper: https://arxiv.org/abs/2504.00742
- AAM article: https://link.springer.com/article/10.1186/s13636-023-00278-7
- MultiTracks.com terms reference: https://www.multitracks.com/terms/
