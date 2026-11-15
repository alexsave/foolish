# Lyria sampler instruments

These samples are cut from AI-generated (Lyria) clips, or from Demucs stems of them.
They are used for the replication work only.
Whether they may ship is undecided.
The owner tested notes cut from these samples with Google's SynthID check: they are detected as Lyria-generated, also after cutting single notes and arranging them with reverb.
The watermark is treated as a provenance label; nothing here tries to remove it.
The `_flat` sample sets are the same cuts with their own vibrato and level movement removed (tools/samples/flatten_sample.py); their source rows are the ones below.
The wavs live in `/Users/alex/Dev/soundtrack-samples/lyria/<instrument>/` and the manifests in `samples/manifests/lyria_<instrument>.txt`.
Cents corrected is the pitch shift applied to land on equal temperament (the opposite of the measured offset).
A window is in seconds of the original clip; for the second half of wa_22 it already includes the 15.0988 s offset.

| file | source | window s | note | cents corrected | purity | attack | layer |
|---|---|---|---|---|---|---|---|
| trumpet_D4_62_L0.wav | ma_19_clip_solo_trumpet_low_soft | 0.832-1.793 | D4 | -15.4 | 0.994 | natural | 0 |
| trumpet_F4_65_L0.wav | ma_21_clip_solo_trumpet_high_soft | 8.772-9.763 | F4 | -11.5 | 0.998 | natural | 0 |
| trumpet_F4_65_L0_rr1.wav | ma_19_clip_solo_trumpet_low_soft | 1.873-2.793 | F4 | -9.1 | 0.993 | none (legato) | 0 |
| trumpet_G4_67_L0.wav | ma_19_clip_solo_trumpet_low_soft | 6.873-7.773 | G4 | -11.5 | 0.991 | none (legato) | 0 |
| trumpet_A#4_70_L0.wav | ma_19_clip_solo_trumpet_low_soft | 3.363-4.793 | A#4 | -11.5 | 0.994 | none (legato) | 0 |
| trumpet_C5_72_L0.wav | ma_21_clip_solo_trumpet_high_soft | 5.083-6.103 | C5 | +3.1 | 0.982 | none (legato) | 0 |
| trumpet_D5_74_L0.wav | ma_19_clip_solo_trumpet_low_soft | 9.963-10.813 | D5 | +2.2 | 0.989 | none (legato) | 0 |
| trumpet_D#5_75_L0.wav | ma_21_clip_solo_trumpet_high_soft | 1.283-3.463 | D#5 | -15.3 | 0.992 | none (legato) | 0 |
| trumpet_D4_62_L1.wav | ma_20_clip_solo_trumpet_low_loud | 0.073-0.613 | D4 | -16.8 | 0.968 | none (legato) | 1 |
| trumpet_D#4_63_L1.wav | ma_22_clip_solo_trumpet_high_loud | 7.793-8.723 | D#4 | -11.9 | 0.994 | none (legato) | 1 |
| trumpet_D#4_63_L1_rr1.wav | Demucs other stem of ma_23 | 6.953-7.573 | D#4 | +4.4 | 0.988 | none (legato) | 1 |
| trumpet_F4_65_L1.wav | ma_20_clip_solo_trumpet_low_loud | 23.252-24.803 | F4 | -4.1 | 0.998 | natural | 1 |
| trumpet_G4_67_L1.wav | ma_20_clip_solo_trumpet_low_loud | 9.193-10.583 | G4 | -10.5 | 0.994 | none (legato) | 1 |
| trumpet_A#4_70_L1.wav | Demucs other stem of ma_23 | 16.123-16.873 | A#4 | -7.1 | 0.992 | none (legato) | 1 |
| trumpet_C5_72_L1.wav | ma_18_clip_solo_trumpet_notes | 16.033-19.983 | C5 | +3.7 | 0.995 | none (legato) | 1 |
| trumpet_C5_72_L1_rr1.wav | ma_22_clip_solo_trumpet_high_loud | 0.793-1.543 | C5 | -5.0 | 0.992 | none (legato) | 1 |
| trumpet_C#5_73_L1.wav | Demucs other stem of ma_23 | 2.953-3.603 | C#5 | +4.4 | 0.994 | none (legato) | 1 |
| trumpet_F5_77_L1.wav | Demucs other stem of ma_23 | 1.153-1.893 | F5 | -14.7 | 0.986 | none (legato) | 1 |
| domra_A3_57.wav | Demucs other stem of mo_18 | 1.032-1.682 | A3 | +2.9 | 0.998 | natural | 0 |
| domra_A3_57_rr1.wav | Demucs other stem of mo_18 | 2.022-2.342 | A3 | +2.4 | 0.998 | natural | 0 |
| domra_B3_59.wav | Demucs other stem of mo_18 | 14.012-14.342 | B3 | +1.3 | 0.999 | natural | 0 |
| domra_B3_59_rr1.wav | Demucs other stem of mo_18 | 15.022-15.372 | B3 | +0.4 | 0.998 | natural | 0 |
| domra_E5_76.wav | Demucs other stem of mo_18 | 25.022-25.400 | E5 | +1.1 | 0.996 | natural | 0 |
| spanish_guitar_B2_47.wav | ma_17_clip_solo_spanish_guitar_notes | 13.792-14.262 | B2 | -1.8 | 0.997 | natural | 0 |
| spanish_guitar_B2_47_rr1.wav | ma_17_clip_solo_spanish_guitar_notes | 14.272-14.772 | B2 | -4.4 | 0.996 | natural | 0 |
| cello_D2_38.wav | Demucs other stem of wa_21 | 25.353-25.863 | D2 | +12.6 | 0.998 | none (legato) | 0 |
| cello_A#2_46.wav | wa_19_clip_solo_cello_low_loud | 15.952-17.793 | A#2 | +0.6 | 0.996 | natural | 0 |
| cello_A#2_46_rr1.wav | wa_19_clip_solo_cello_low_loud | 0.163-2.053 | A#2 | -4.9 | 0.997 | none (legato) | 0 |
| cello_B2_47.wav | Demucs other stem of wa_22 (second half only) | 15.142-16.982 | B2 | -0.5 | 0.998 | none (legato) | 0 |
| cello_D3_50.wav | wa_17_clip_solo_cello_notes | 16.413-17.393 | D3 | -8.9 | 0.997 | none (legato) | 0 |
| cello_D3_50_rr1.wav | wa_17_clip_solo_cello_notes | 0.363-1.323 | D3 | -5.6 | 0.996 | none (legato) | 0 |
| cello_E3_52.wav | Demucs other stem of wa_21 | 9.063-9.863 | E3 | -15.4 | 0.997 | none (legato) | 0 |
| cello_F3_53.wav | wa_17_clip_solo_cello_notes | 25.443-27.313 | F3 | +0.3 | 0.998 | none (legato) | 0 |
| cello_F3_53_rr1.wav | wa_19_clip_solo_cello_low_loud | 2.183-4.033 | F3 | -8.8 | 0.998 | none (legato) | 0 |
| cello_F#3_54.wav | Demucs other+vocals stem of wa_18 | 0.063-0.643 | F#3 | -0.6 | 0.998 | none (legato) | 0 |
| cello_F#3_54_rr1.wav | wa_19_clip_solo_cello_low_loud | 4.103-5.903 | F#3 | +7.5 | 0.996 | none (legato) | 0 |
| cello_G3_55.wav | wa_17_clip_solo_cello_notes | 27.403-29.303 | G3 | +2.8 | 0.997 | none (legato) | 0 |
| cello_G3_55_rr1.wav | wa_17_clip_solo_cello_notes | 11.353-13.293 | G3 | -1.8 | 0.996 | none (legato) | 0 |
| cello_G#3_56.wav | wa_19_clip_solo_cello_low_loud | 7.063-8.013 | G#3 | -10.9 | 0.996 | none (legato) | 0 |
| cello_G#3_56_rr1.wav | Demucs other+vocals stem of wa_18 | 0.753-1.643 | G#3 | -0.8 | 0.996 | none (legato) | 0 |
| cello_A3_57.wav | wa_20_clip_solo_cello_mid_soft | 17.653-18.603 | A3 | -3.8 | 0.998 | none (legato) | 0 |
| cello_A3_57_rr1.wav | Demucs other stem of wa_21 | 10.103-12.893 | A3 | -7.3 | 0.996 | none (legato) | 0 |
| cello_A#3_58.wav | wa_17_clip_solo_cello_notes | 23.332-25.253 | A#3 | -11.2 | 0.997 | natural | 0 |
| cello_A#3_58_rr1.wav | wa_17_clip_solo_cello_notes | 7.453-9.213 | A#3 | -8.3 | 0.996 | none (legato) | 0 |
| cello_B3_59.wav | Demucs other stem of wa_21 | 1.093-1.973 | B3 | -12.7 | 0.984 | none (legato) | 0 |
| cello_C4_60.wav | wa_20_clip_solo_cello_mid_soft | 12.643-13.613 | C4 | -2.7 | 0.994 | none (legato) | 0 |
| cello_C4_60_rr1.wav | wa_20_clip_solo_cello_mid_soft | 4.673-6.483 | C4 | -6.4 | 0.989 | none (legato) | 0 |
| cello_C#4_61.wav | wa_20_clip_solo_cello_mid_soft | 13.683-14.613 | C#4 | +1.8 | 0.991 | none (legato) | 0 |
| cello_D4_62.wav | wa_20_clip_solo_cello_mid_soft | 7.643-8.633 | D4 | -0.8 | 0.991 | none (legato) | 0 |
| haegeum_C4_60.wav | se_17_clip_solo_haegeum_notes | 29.823-30.703 | C4 | +8.7 | 0.998 | none (legato) | 0 |
| haegeum_C4_60_rr1.wav | se_17_clip_solo_haegeum_notes | 13.813-14.763 | C4 | +1.2 | 0.998 | none (legato) | 0 |
| haegeum_D4_62.wav | se_17_clip_solo_haegeum_notes | 23.813-24.743 | D4 | +1.1 | 0.998 | none (legato) | 0 |
| haegeum_D4_62_rr1.wav | se_17_clip_solo_haegeum_notes | 15.803-16.723 | D4 | +3.4 | 0.997 | none (legato) | 0 |
| haegeum_E4_64.wav | se_17_clip_solo_haegeum_notes | 21.813-23.703 | E4 | +2.3 | 0.993 | none (legato) | 0 |
| haegeum_F4_65.wav | se_17_clip_solo_haegeum_notes | 19.833-21.733 | F4 | +7.4 | 0.989 | none (legato) | 0 |
| haegeum_G4_67.wav | se_17_clip_solo_haegeum_notes | 24.833-25.733 | G4 | +4.9 | 0.997 | none (legato) | 0 |
| haegeum_G4_67_rr1.wav | se_17_clip_solo_haegeum_notes | 16.813-19.723 | G4 | +3.3 | 0.988 | none (legato) | 0 |
| haegeum_A4_69.wav | se_17_clip_solo_haegeum_notes | 28.793-29.723 | A4 | -3.8 | 0.998 | none (legato) | 0 |
| haegeum_A4_69_rr1.wav | se_21_clip_solo_haegeum_high_loud | 8.683-9.563 | A4 | +0.5 | 0.998 | none (legato) | 0 |
| haegeum_B4_71.wav | se_23_clip_solo_haegeum_high_soft | 1.293-2.123 | B4 | -1.9 | 0.986 | none (legato) | 0 |
| haegeum_B4_71_rr1.wav | se_17_clip_solo_haegeum_notes | 9.803-11.203 | B4 | -3.6 | 0.980 | none (legato) | 0 |
| haegeum_C5_72.wav | se_17_clip_solo_haegeum_notes | 7.263-7.713 | C5 | +2.9 | 0.997 | none (legato) | 0 |
| haegeum_C5_72_rr1.wav | se_23_clip_solo_haegeum_high_soft | 14.163-15.943 | C5 | -5.4 | 0.994 | none (legato) | 0 |
| haegeum_C#5_73.wav | se_19_clip_solo_haegeum_low_loud | 6.923-7.373 | C#5 | -6.1 | 0.998 | none (legato) | 0 |
| haegeum_C#5_73_rr1.wav | se_23_clip_solo_haegeum_high_soft | 12.153-14.103 | C#5 | -10.5 | 0.982 | none (legato) | 0 |
| haegeum_D5_74.wav | se_17_clip_solo_haegeum_notes | 7.793-9.203 | D5 | +0.5 | 0.971 | none (legato) | 0 |
| haegeum_D5_74_rr1.wav | se_17_clip_solo_haegeum_notes | 3.793-5.213 | D5 | -6.0 | 0.966 | none (legato) | 0 |
| haegeum_D#5_75.wav | se_17_clip_solo_haegeum_notes | 2.803-3.733 | D#5 | -4.4 | 0.998 | none (legato) | 0 |
| haegeum_D#5_75_rr1.wav | se_19_clip_solo_haegeum_low_loud | 8.903-10.843 | D#5 | -10.3 | 0.978 | none (legato) | 0 |
| haegeum_E5_76.wav | se_19_clip_solo_haegeum_low_loud | 20.863-21.803 | E5 | -3.2 | 0.997 | none (legato) | 0 |
| haegeum_E5_76_rr1.wav | se_19_clip_solo_haegeum_low_loud | 16.853-17.863 | E5 | -8.2 | 0.942 | none (legato) | 0 |
| haegeum_F5_77.wav | se_17_clip_solo_haegeum_notes | 1.313-2.743 | F5 | -4.7 | 0.960 | none (legato) | 0 |
| haegeum_F#5_78.wav | se_21_clip_solo_haegeum_high_loud | 13.653-14.593 | F#5 | -2.1 | 0.996 | none (legato) | 0 |
| haegeum_F#5_78_rr1.wav | se_21_clip_solo_haegeum_high_loud | 16.683-18.633 | F#5 | -12.3 | 0.989 | none (legato) | 0 |
| haegeum_G5_79.wav | se_21_clip_solo_haegeum_high_loud | 14.653-16.613 | G5 | -2.9 | 0.995 | none (legato) | 0 |

75 samples.

## v6 flat sets (flat-v6 branch)

Lyria-derived. The `_flat_v6` sets are the same cuts as the rows above, flattened from the SOURCE cuts (not from the v5 flat files) with `tools/instruments/flatten_v6.py`: the v5 flattening, then a sub-cent pitch pass, then per-harmonic level flattening and harmonic phase lock (a sustain with no pitch, level or per-harmonic modulation).
Files: `/Users/alex/Dev/soundtrack-samples/lyria/{cello,haegeum,trumpet}_flat_v6/` (68 wavs, same names as the v5 flat files), manifests `samples/manifests/lyria_{cello,haegeum,trumpet}_flat_v6.txt`, per-sample before and after in `/Users/alex/Music/foolish_cities_v5/analysis/lyria_samples/<instrument>_flat_v6.md`.
The v5 files and manifests are untouched.
