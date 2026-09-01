// Reference-output generator for DSPMonitor's Swift filter audit.
//
// Reads raw little-endian f64 samples (mono), runs a camilladsp filter chosen
// via CLI arg, writes the resulting samples back as raw little-endian f64.
//
// Modes:
//   biquad   <a1> <a2> <b0> <b1> <b2> <samplerate> <chunk_size> <input> <output>
//   gain     <gain_db> <inverted:0|1> <mute:0|1> <chunk_size> <input> <output>
//   volume   <current_volume_db> <mute:0|1> <samplerate> <chunk_size> <input> <output>
//            (ramp_time_ms is hardcoded to 0 — both ends use instant gain
//             update so the test can compare bit-for-bit without ramp state.)
//   loudness <current_volume_db> <reference_level_db> <high_boost_db> <low_boost_db>
//            <attenuate_mid:0|1> <samplerate> <chunk_size> <input> <output>

use camillalib::ProcessingParameters;
use camillalib::audiochunk::AudioChunk;
use camillalib::config::{LoudnessFader, LoudnessParameters};
use camillalib::filters::Filter;
use camillalib::filters::basicfilters::{Gain, Volume};
use camillalib::filters::biquad::{Biquad, BiquadCoefficients};
use camillalib::filters::fftconv::FftConv;
use camillalib::filters::loudness::Loudness;
use camillalib::processors::Processor;
use camillalib::processors::compressor::Compressor;
use camillalib::processors::noisegate::NoiseGate;
use camillalib::processors::race::RACE;
use std::sync::Arc;

use std::convert::TryInto;
use std::fs::File;
use std::io::{BufReader, BufWriter, Read, Write};

const BPS: usize = 8;

fn read_f64(path: &str) -> Vec<f64> {
    let mut buf = [0u8; BPS];
    let mut out = Vec::new();
    let mut r = BufReader::new(File::open(path).expect("open input"));
    while r.read(&mut buf).unwrap() == BPS {
        out.push(f64::from_le_bytes(buf.as_slice().try_into().unwrap()));
    }
    out
}

fn write_f64(path: &str, data: &[f64]) {
    let mut w = BufWriter::new(File::create(path).expect("create output"));
    for v in data {
        w.write_all(&v.to_le_bytes()).unwrap();
    }
}

fn run_filter<F: Filter + ?Sized>(filter: &mut F, input: Vec<f64>, chunk_size: usize) -> Vec<f64> {
    let mut samples = input;
    for chunk in samples.chunks_mut(chunk_size) {
        filter.process_waveform(chunk);
    }
    samples
}

fn run_filter_slice<F: Filter + ?Sized>(filter: &mut F, samples: &mut [f64], chunk_size: usize) {
    for chunk in samples.chunks_mut(chunk_size) {
        filter.process_waveform(chunk);
    }
}

fn make_peaking_biquad(samplerate: usize, freq: f64, q: f64, gain_db: f64) -> Biquad {
    let a_gain = 10.0f64.powf(gain_db / 40.0);
    let w0 = 2.0 * std::f64::consts::PI * freq / (samplerate as f64);
    let alpha = w0.sin() / (2.0 * q);
    let cos_w0 = w0.cos();

    let b0 = 1.0 + alpha * a_gain;
    let b1 = -2.0 * cos_w0;
    let b2 = 1.0 - alpha * a_gain;
    let a0 = 1.0 + alpha / a_gain;
    let a1 = -2.0 * cos_w0;
    let a2 = 1.0 - alpha / a_gain;

    let coeffs = BiquadCoefficients::new(a1 / a0, a2 / a0, b0 / a0, b1 / a0, b2 / a0);
    Biquad::new("bq", samplerate, coeffs)
}

fn make_sinc_coeffs(length: usize) -> Vec<f64> {
    let mut values = vec![0.0f64; length];
    for idx in 0..length {
        let x = idx as f64 - (length - 1) as f64 * 0.5;
        let sinc = if x == 0.0 {
            1.0
        } else {
            (std::f64::consts::PI * x).sin() / (std::f64::consts::PI * x)
        };
        values[idx] = sinc;
    }
    values
}

const PRE_FREQS: [f64; 16] = [
    120.0, 220.0, 350.0, 500.0, 700.0, 900.0, 1200.0, 1600.0,
    1800.0, 2200.0, 2800.0, 3200.0, 3800.0, 4500.0, 6200.0, 8000.0,
];
const PRE_QS: [f64; 16] = [
    0.70, 0.75, 0.80, 0.90, 1.00, 1.10, 0.95, 1.05,
    1.10, 0.90, 0.95, 1.00, 0.85, 0.80, 0.75, 0.70,
];
const POST_FREQS: [f64; 16] = [
    140.0, 260.0, 400.0, 560.0, 760.0, 980.0, 1300.0, 1700.0,
    2100.0, 2500.0, 3000.0, 3600.0, 4200.0, 5200.0, 6800.0, 9200.0,
];
const POST_QS: [f64; 16] = [
    0.72, 0.78, 0.83, 0.92, 1.02, 1.08, 0.98, 1.06,
    1.00, 0.94, 0.92, 0.88, 0.84, 0.80, 0.76, 0.72,
];

fn bench_filter<F: Filter + ?Sized>(filter: &mut F, chunk_size: usize, iters: usize) {
    let mut samples = vec![0.0f64; chunk_size];
    for _ in 0..100 {
        run_filter_slice(filter, &mut samples, chunk_size);
    }
    let start = std::time::Instant::now();
    for _ in 0..iters {
        run_filter_slice(filter, &mut samples, chunk_size);
    }
    let elapsed_ns = start.elapsed().as_nanos() as f64;
    let ns_per_frame = elapsed_ns / ((chunk_size * iters) as f64);
    println!("{ns_per_frame:.3}");
}

fn main() {
    let args: Vec<_> = std::env::args().collect();
    if args.len() < 2 {
        eprintln!("usage: {} <mode> ... — see source for mode args", args[0]);
        std::process::exit(2);
    }
    let bench_iters: usize = args
        .iter()
        .find_map(|a| a.strip_prefix("--bench="))
        .map(|n| n.parse().expect("--bench= expects an integer"))
        .unwrap_or(0);
    match args[1].as_str() {
        "bench" => {
            if args.len() < 3 {
                eprintln!("usage: {} bench <biquad|diffeq|conv|pipeline_biquad|pipeline_biquad_conv> [args...]", args[0]);
                std::process::exit(2);
            }
            let filter_type = &args[2];
            match filter_type.as_str() {
                "biquad" => {
                    let samplerate: usize = args.get(3).and_then(|s| s.parse().ok()).unwrap_or(44100);
                    let chunk_size: usize = args.get(4).and_then(|s| s.parse().ok()).unwrap_or(1024);
                    let iters: usize = args.get(5).and_then(|s| s.parse().ok()).unwrap_or(5000);
                    let coeffs = BiquadCoefficients::new(
                        -0.1462978543780541,
                        0.005350765548905586,
                        0.21476322779271284,
                        0.4295264555854257,
                        0.21476322779271284,
                    );
                    let mut filter = Biquad::new("test", samplerate, coeffs);
                    bench_filter(&mut filter, chunk_size, iters);
                }
                "diffeq" => {
                    let _samplerate: usize = args.get(3).and_then(|s| s.parse().ok()).unwrap_or(44100);
                    let chunk_size: usize = args.get(4).and_then(|s| s.parse().ok()).unwrap_or(1024);
                    let iters: usize = args.get(5).and_then(|s| s.parse().ok()).unwrap_or(5000);
                    let a = vec![1.0, -0.1462978543780541, 0.005350765548905586];
                    let b = vec![0.21476322779271284, 0.4295264555854257, 0.21476322779271284];
                    let mut filter = camillalib::filters::diffeq::DiffEq::new("test", a, b);
                    bench_filter(&mut filter, chunk_size, iters);
                }
                "conv" => {
                    let taps: usize = args.get(3).and_then(|s| s.parse().ok()).unwrap_or(1024);
                    let chunk_size: usize = args.get(4).and_then(|s| s.parse().ok()).unwrap_or(1024);
                    let iters: usize = args.get(5).and_then(|s| s.parse().ok()).unwrap_or(5000);
                    let coeffs = vec![0.0f64; taps];
                    let mut filter = FftConv::new("test", chunk_size, &coeffs);
                    bench_filter(&mut filter, chunk_size, iters);
                }
                "realfft" => {
                    let length: usize = args.get(3).and_then(|s| s.parse().ok()).unwrap_or(1024);
                    let iters: usize = args.get(4).and_then(|s| s.parse().ok()).unwrap_or(5000);
                    let mut planner = realfft::RealFftPlanner::<f64>::new();
                    let r2c = planner.plan_fft_forward(length);
                    let c2r = planner.plan_fft_inverse(length);
                    let mut in_real = r2c.make_input_vec();
                    let mut spec = r2c.make_output_vec();
                    let mut out_real = c2r.make_output_vec();
                    for i in 0..length {
                        in_real[i] = (2.0 * std::f64::consts::PI * 1000.0 * (i as f64) / 48000.0).sin();
                    }
                    // warm-up
                    for _ in 0..50 {
                        r2c.process(&mut in_real, &mut spec).unwrap();
                        c2r.process(&mut spec, &mut out_real).unwrap();
                    }
                    let start = std::time::Instant::now();
                    for _ in 0..iters {
                        r2c.process(&mut in_real, &mut spec).unwrap();
                        c2r.process(&mut spec, &mut out_real).unwrap();
                        std::hint::black_box(&out_real);
                    }
                    let elapsed_ns = start.elapsed().as_nanos() as f64;
                    let ns_per_roundtrip = elapsed_ns / (iters as f64);
                    println!("{ns_per_roundtrip:.3}");
                }
                "pipeline_biquad" => {
                    let multi = args.get(3).map(|s| s == "multi").unwrap_or(false);
                    let chunk_size: usize = args.get(4).and_then(|s| s.parse().ok()).unwrap_or(1024);
                    let iters: usize = args.get(5).and_then(|s| s.parse().ok()).unwrap_or(200);
                    let samplerate = 48000;

                    let mut pre_filters: Vec<Vec<Biquad>> = (0..4)
                        .map(|_| {
                            (0..16)
                                .map(|i| make_peaking_biquad(samplerate, PRE_FREQS[i], PRE_QS[i], 1.5))
                                .collect()
                        })
                        .collect();

                    let mut post_filters: Vec<Vec<Biquad>> = (0..2)
                        .map(|_| {
                            (0..16)
                                .map(|i| make_peaking_biquad(samplerate, POST_FREQS[i], POST_QS[i], 1.5))
                                .collect()
                        })
                        .collect();

                    let mut in_chunks = vec![vec![0.0f64; chunk_size]; 4];
                    for ch in 0..4 {
                        for t in 0..chunk_size {
                            in_chunks[ch][t] = (2.0 * std::f64::consts::PI * 1000.0 * (t as f64) / (samplerate as f64)).sin();
                        }
                    }
                    let mut out_chunks = vec![vec![0.0f64; chunk_size]; 2];
                    let gain_factor = 10.0f64.powf(-6.0 / 20.0);

                    let run_step = |pre: &mut Vec<Vec<Biquad>>, post: &mut Vec<Vec<Biquad>>, in_c: &mut Vec<Vec<f64>>, out_c: &mut Vec<Vec<f64>>| {
                        if multi {
                            use rayon::prelude::*;
                            pre.par_iter_mut().zip(in_c.par_iter_mut()).for_each(|(flist, ch_data)| {
                                for f in flist {
                                    f.process_waveform(ch_data);
                                }
                            });
                            for t in 0..chunk_size {
                                out_c[0][t] = in_c[0][t] + gain_factor * in_c[2][t];
                                out_c[1][t] = in_c[1][t] + gain_factor * in_c[3][t];
                            }
                            post.par_iter_mut().zip(out_c.par_iter_mut()).for_each(|(flist, ch_data)| {
                                for f in flist {
                                    f.process_waveform(ch_data);
                                }
                            });
                        } else {
                            for (flist, ch_data) in pre.iter_mut().zip(in_c.iter_mut()) {
                                for f in flist {
                                    f.process_waveform(ch_data);
                                }
                            }
                            for t in 0..chunk_size {
                                out_c[0][t] = in_c[0][t] + gain_factor * in_c[2][t];
                                out_c[1][t] = in_c[1][t] + gain_factor * in_c[3][t];
                            }
                            for (flist, ch_data) in post.iter_mut().zip(out_c.iter_mut()) {
                                for f in flist {
                                    f.process_waveform(ch_data);
                                }
                            }
                        }
                    };

                    for _ in 0..50 {
                        run_step(&mut pre_filters, &mut post_filters, &mut in_chunks, &mut out_chunks);
                    }

                    let start = std::time::Instant::now();
                    for _ in 0..iters {
                        run_step(&mut pre_filters, &mut post_filters, &mut in_chunks, &mut out_chunks);
                    }
                    let elapsed_ms = (start.elapsed().as_nanos() as f64) / 1e6 / (iters as f64);
                    println!("{elapsed_ms:.3}");
                }
                "pipeline_biquad_conv" => {
                    let multi = args.get(3).map(|s| s == "multi").unwrap_or(false);
                    let chunk_size: usize = args.get(4).and_then(|s| s.parse().ok()).unwrap_or(1024);
                    let iters: usize = args.get(5).and_then(|s| s.parse().ok()).unwrap_or(10);
                    let samplerate = 48000;

                    let conv32k_coeffs = make_sinc_coeffs(32768);
                    let conv64k_coeffs = make_sinc_coeffs(65536);

                    let mut pre_bqs: Vec<Vec<Biquad>> = (0..4)
                        .map(|_| {
                            (0..16)
                                .map(|i| make_peaking_biquad(samplerate, PRE_FREQS[i], PRE_QS[i], 1.5))
                                .collect()
                        })
                        .collect();
                    let mut pre_convs: Vec<(FftConv, FftConv)> = (0..4)
                        .map(|_| {
                            (
                                FftConv::new("conv1", chunk_size, &conv32k_coeffs),
                                FftConv::new("conv2", chunk_size, &conv64k_coeffs),
                            )
                        })
                        .collect();

                    let mut post_bqs: Vec<Vec<Biquad>> = (0..2)
                        .map(|_| {
                            (0..16)
                                .map(|i| make_peaking_biquad(samplerate, POST_FREQS[i], POST_QS[i], 1.5))
                                .collect()
                        })
                        .collect();
                    let mut post_convs: Vec<(FftConv, FftConv)> = (0..2)
                        .map(|_| {
                            (
                                FftConv::new("conv1", chunk_size, &conv32k_coeffs),
                                FftConv::new("conv2", chunk_size, &conv64k_coeffs),
                            )
                        })
                        .collect();

                    let mut in_chunks = vec![vec![0.0f64; chunk_size]; 4];
                    for ch in 0..4 {
                        for t in 0..chunk_size {
                            in_chunks[ch][t] = (2.0 * std::f64::consts::PI * 1000.0 * (t as f64) / (samplerate as f64)).sin();
                        }
                    }
                    let mut out_chunks = vec![vec![0.0f64; chunk_size]; 2];
                    let gain_factor = 10.0f64.powf(-6.0 / 20.0);

                    let run_step = |pre_b: &mut Vec<Vec<Biquad>>,
                                        pre_c: &mut Vec<(FftConv, FftConv)>,
                                        post_b: &mut Vec<Vec<Biquad>>,
                                        post_c: &mut Vec<(FftConv, FftConv)>,
                                        in_c: &mut Vec<Vec<f64>>,
                                        out_c: &mut Vec<Vec<f64>>| {
                        if multi {
                            use rayon::prelude::*;
                            pre_b
                                .par_iter_mut()
                                .zip(pre_c.par_iter_mut())
                                .zip(in_c.par_iter_mut())
                                .for_each(|((b_list, c_pair), ch_data)| {
                                    for f in b_list {
                                        f.process_waveform(ch_data);
                                    }
                                    c_pair.0.process_waveform(ch_data);
                                    c_pair.1.process_waveform(ch_data);
                                });
                            for t in 0..chunk_size {
                                out_c[0][t] = in_c[0][t] + gain_factor * in_c[2][t];
                                out_c[1][t] = in_c[1][t] + gain_factor * in_c[3][t];
                            }
                            post_b
                                .par_iter_mut()
                                .zip(post_c.par_iter_mut())
                                .zip(out_c.par_iter_mut())
                                .for_each(|((b_list, c_pair), ch_data)| {
                                    for f in b_list {
                                        f.process_waveform(ch_data);
                                    }
                                    c_pair.0.process_waveform(ch_data);
                                    c_pair.1.process_waveform(ch_data);
                                });
                        } else {
                            for ((b_list, c_pair), ch_data) in pre_b.iter_mut().zip(pre_c.iter_mut()).zip(in_c.iter_mut()) {
                                for f in b_list {
                                    f.process_waveform(ch_data);
                                }
                                c_pair.0.process_waveform(ch_data);
                                c_pair.1.process_waveform(ch_data);
                            }
                            for t in 0..chunk_size {
                                out_c[0][t] = in_c[0][t] + gain_factor * in_c[2][t];
                                out_c[1][t] = in_c[1][t] + gain_factor * in_c[3][t];
                            }
                            for ((b_list, c_pair), ch_data) in post_b.iter_mut().zip(post_c.iter_mut()).zip(out_c.iter_mut()) {
                                for f in b_list {
                                    f.process_waveform(ch_data);
                                }
                                c_pair.0.process_waveform(ch_data);
                                c_pair.1.process_waveform(ch_data);
                            }
                        }
                    };

                    for _ in 0..20 {
                        run_step(&mut pre_bqs, &mut pre_convs, &mut post_bqs, &mut post_convs, &mut in_chunks, &mut out_chunks);
                    }

                    let start = std::time::Instant::now();
                    for _ in 0..iters {
                        run_step(&mut pre_bqs, &mut pre_convs, &mut post_bqs, &mut post_convs, &mut in_chunks, &mut out_chunks);
                    }
                    let elapsed_ms = (start.elapsed().as_nanos() as f64) / 1e6 / (iters as f64);
                    println!("{elapsed_ms:.3}");
                }
                other => {
                    eprintln!("unknown bench filter type: {other}");
                    std::process::exit(2);
                }
            }
            return;
        }
        "biquad" => {
            // <a1> <a2> <b0> <b1> <b2> <samplerate> <chunk_size> <input> <output>
            assert!(args.len() == 11, "biquad needs 9 trailing args");
            let a1: f64 = args[2].parse().unwrap();
            let a2: f64 = args[3].parse().unwrap();
            let b0: f64 = args[4].parse().unwrap();
            let b1: f64 = args[5].parse().unwrap();
            let b2: f64 = args[6].parse().unwrap();
            let samplerate: usize = args[7].parse().unwrap();
            let chunk_size: usize = args[8].parse().unwrap();
            let in_path = &args[9];
            let out_path = &args[10];
            let coeffs = BiquadCoefficients::new(a1, a2, b0, b1, b2);
            let mut filter = Biquad::new("test", samplerate, coeffs);
            let input = read_f64(in_path);
            let output = run_filter(&mut filter, input, chunk_size);
            write_f64(out_path, &output);
        }
        "gain" => {
            // <gain_db> <inverted:0|1> <mute:0|1> <chunk_size> <input> <output>
            assert!(args.len() == 8, "gain needs 6 trailing args");
            let gain_db: f64 = args[2].parse().unwrap();
            let inverted: bool = args[3] == "1";
            let mute: bool = args[4] == "1";
            let chunk_size: usize = args[5].parse().unwrap();
            let in_path = &args[6];
            let out_path = &args[7];
            let mut filter = Gain::new("test", gain_db, inverted, mute, false);
            let input = read_f64(in_path);
            let output = run_filter(&mut filter, input, chunk_size);
            write_f64(out_path, &output);
        }
        "volume" => {
            // <current_volume_db> <mute:0|1> <samplerate> <chunk_size> <input> <output>
            // ramp_time_ms is forced to 0 so the filter takes the constant-gain
            // branch on every chunk — this lets the Swift comparison test
            // ignore ramp state.
            assert!(args.len() == 8, "volume needs 6 trailing args");
            let current_volume: f32 = args[2].parse().unwrap();
            let mute: bool = args[3] == "1";
            let samplerate: usize = args[4].parse().unwrap();
            let chunk_size: usize = args[5].parse().unwrap();
            let in_path = &args[6];
            let out_path = &args[7];
            let processing_params = Arc::new(ProcessingParameters::new(
                &[current_volume, 0.0, 0.0, 0.0, 0.0],
                &[mute, false, false, false, false],
            ));
            let mut filter = Volume::new(
                "test",
                /*ramp_time_ms=*/ 0.0,
                /*limit=*/ 50.0,
                current_volume,
                mute,
                chunk_size,
                samplerate,
                processing_params,
                /*fader=*/ 0,
            );
            let input = read_f64(in_path);
            let output = run_filter(&mut filter, input, chunk_size);
            write_f64(out_path, &output);
        }
        "loudness" => {
            // <current_volume_db> <reference_level_db> <high_boost_db>
            //   <low_boost_db> <attenuate_mid:0|1> <samplerate> <chunk_size>
            //   <input> <output>
            assert!(args.len() == 11, "loudness needs 9 trailing args");
            let current_volume: f32 = args[2].parse().unwrap();
            let reference_level: f32 = args[3].parse().unwrap();
            let high_boost: f32 = args[4].parse().unwrap();
            let low_boost: f32 = args[5].parse().unwrap();
            let attenuate_mid: bool = args[6] == "1";
            let samplerate: usize = args[7].parse().unwrap();
            let chunk_size: usize = args[8].parse().unwrap();
            let in_path = &args[9];
            let out_path = &args[10];
            let processing_params = Arc::new(ProcessingParameters::new(
                &[current_volume, 0.0, 0.0, 0.0, 0.0],
                &[false, false, false, false, false],
            ));
            // Make sure current_volume on the shared params reflects what we
            // pass in — Loudness::process_waveform reads
            // processing_params.current_volume(fader), not target.
            processing_params.set_current_volume(0, current_volume);
            let conf = LoudnessParameters {
                reference_level,
                high_boost: Some(high_boost),
                low_boost: Some(low_boost),
                fader: Some(LoudnessFader::Main),
                attenuate_mid: Some(attenuate_mid),
            };
            let mut filter = Loudness::from_config("test", conf, samplerate, processing_params);
            let input = read_f64(in_path);
            let output = run_filter(&mut filter, input, chunk_size);
            write_f64(out_path, &output);
        }
        "conv" => {
            // <chunk_size> <coeffs_path> <input> <output> [--bench=N]
            assert!(args.len() >= 6, "conv needs at least 4 trailing args");
            let chunk_size: usize = args[2].parse().unwrap();
            let coeffs_path = &args[3];
            let in_path = &args[4];
            let out_path = &args[5];
            let coeffs = read_f64(coeffs_path);
            let mut filter = FftConv::new("test", chunk_size, &coeffs);
            let input = read_f64(in_path);
            if bench_iters == 0 {
                let output = run_filter(&mut filter, input, chunk_size);
                write_f64(out_path, &output);
            } else {
                let mut samples = input.clone();
                run_filter_slice(&mut filter, &mut samples, chunk_size);
                let start = std::time::Instant::now();
                for _ in 0..bench_iters {
                    samples.copy_from_slice(&input);
                    run_filter_slice(&mut filter, &mut samples, chunk_size);
                }
                let elapsed_ns = start.elapsed().as_nanos();
                let frames_per_iter = samples.len();
                eprintln!(
                    "BENCH_NS_TOTAL={elapsed_ns}  BENCH_OUT_FRAMES_PER_ITER={frames_per_iter}  BENCH_ITERS={bench_iters}  BENCH_MODE=conv"
                );
            }
        }
        "delay" => {
            // delay <delay_value> <unit:ms|us|samples|mm> <subsample:0|1> <samplerate> <chunk_size> <input> <output>
            assert!(args.len() == 9, "delay needs 7 trailing args");
            let delay_val: f64 = args[2].parse().unwrap();
            let unit_str = &args[3];
            let subsample: bool = args[4] == "1";
            let samplerate: usize = args[5].parse().unwrap();
            let chunk_size: usize = args[6].parse().unwrap();
            let in_path = &args[7];
            let out_path = &args[8];
            let unit = match unit_str.as_str() {
                "ms" => camillalib::config::DelayUnit::Milliseconds,
                "us" => camillalib::config::DelayUnit::Microseconds,
                "s" => camillalib::config::DelayUnit::Seconds,
                "samples" => camillalib::config::DelayUnit::Samples,
                "mm" => camillalib::config::DelayUnit::Millimetres,
                _ => panic!("invalid unit"),
            };
            let conf = camillalib::config::DelayParameters {
                delay: delay_val,
                delay_unit: unit,
                subsample: Some(subsample),
            };
            let mut filter =
                camillalib::filters::basicfilters::Delay::from_config("test", samplerate, conf);
            let input = read_f64(in_path);
            let output = run_filter(&mut filter, input, chunk_size);
            write_f64(out_path, &output);
        }
        "biquad_combo" => {
            let combo_type = &args[2];
            let conf = match combo_type.as_str() {
                "butterworth_lowpass" => {
                    let freq: f64 = args[3].parse().unwrap();
                    let order: usize = args[4].parse().unwrap();
                    camillalib::config::BiquadComboParameters::ButterworthLowpass { freq, order }
                }
                "butterworth_highpass" => {
                    let freq: f64 = args[3].parse().unwrap();
                    let order: usize = args[4].parse().unwrap();
                    camillalib::config::BiquadComboParameters::ButterworthHighpass { freq, order }
                }
                "linkwitz_riley_lowpass" => {
                    let freq: f64 = args[3].parse().unwrap();
                    let order: usize = args[4].parse().unwrap();
                    camillalib::config::BiquadComboParameters::LinkwitzRileyLowpass { freq, order }
                }
                "linkwitz_riley_highpass" => {
                    let freq: f64 = args[3].parse().unwrap();
                    let order: usize = args[4].parse().unwrap();
                    camillalib::config::BiquadComboParameters::LinkwitzRileyHighpass { freq, order }
                }
                "tilt" => {
                    let gain: f64 = args[3].parse().unwrap();
                    camillalib::config::BiquadComboParameters::Tilt { gain }
                }
                "five_point_peq" => {
                    let fls: f64 = args[3].parse().unwrap();
                    let qls: f64 = args[4].parse().unwrap();
                    let gls: f64 = args[5].parse().unwrap();
                    let fp1: f64 = args[6].parse().unwrap();
                    let qp1: f64 = args[7].parse().unwrap();
                    let gp1: f64 = args[8].parse().unwrap();
                    let fp2: f64 = args[9].parse().unwrap();
                    let qp2: f64 = args[10].parse().unwrap();
                    let gp2: f64 = args[11].parse().unwrap();
                    let fp3: f64 = args[12].parse().unwrap();
                    let qp3: f64 = args[13].parse().unwrap();
                    let gp3: f64 = args[14].parse().unwrap();
                    let fhs: f64 = args[15].parse().unwrap();
                    let qhs: f64 = args[16].parse().unwrap();
                    let ghs: f64 = args[17].parse().unwrap();
                    camillalib::config::BiquadComboParameters::FivePointPeq {
                        fls,
                        qls,
                        gls,
                        fp1,
                        qp1,
                        gp1,
                        fp2,
                        qp2,
                        gp2,
                        fp3,
                        qp3,
                        gp3,
                        fhs,
                        qhs,
                        ghs,
                    }
                }
                "graphic_equalizer" => {
                    let freq_min: f32 = args[3].parse().unwrap();
                    let freq_max: f32 = args[4].parse().unwrap();
                    let gains: Vec<f32> = args[5].split(',').map(|s| s.parse().unwrap()).collect();
                    let params_json = format!(
                        r#"{{"freq_min":{},"freq_max":{},"gains":{:?}}}"#,
                        freq_min, freq_max, gains
                    );
                    let ge_params = serde_json::from_str(&params_json).unwrap();
                    camillalib::config::BiquadComboParameters::GraphicEqualizer(ge_params)
                }
                _ => panic!("unknown combo type"),
            };
            let last_args_start = args.len() - 4;
            let samplerate: usize = args[last_args_start].parse().unwrap();
            let chunk_size: usize = args[last_args_start + 1].parse().unwrap();
            let in_path = &args[last_args_start + 2];
            let out_path = &args[last_args_start + 3];
            let mut filter = camillalib::filters::biquadcombo::BiquadCombo::from_config(
                "test", samplerate, conf,
            );
            let input = read_f64(in_path);
            let output = run_filter(&mut filter, input, chunk_size);
            write_f64(out_path, &output);
        }
        "diff_eq" => {
            let a: Vec<f64> = args[2].split(',').map(|s| s.parse().unwrap()).collect();
            let b: Vec<f64> = args[3].split(',').map(|s| s.parse().unwrap()).collect();
            let chunk_size: usize = args[4].parse().unwrap();
            let in_path = &args[5];
            let out_path = &args[6];
            let mut filter = camillalib::filters::diffeq::DiffEq::new("test", a, b);
            let input = read_f64(in_path);
            let output = run_filter(&mut filter, input, chunk_size);
            write_f64(out_path, &output);
        }
        "dither" => {
            let dither_type_str = &args[2];
            let bits: usize = args[3].parse().unwrap();
            let mut amp = None;
            let last_args_start = if dither_type_str == "flat" {
                amp = Some(args[4].parse().unwrap());
                5
            } else {
                4
            };
            let chunk_size: usize = args[last_args_start].parse().unwrap();
            let in_path = &args[last_args_start + 1];
            let out_path = &args[last_args_start + 2];
            let conf = match dither_type_str.as_str() {
                "none" => camillalib::config::DitherParameters::None { bits },
                "flat" => camillalib::config::DitherParameters::Flat {
                    bits,
                    amplitude: amp.unwrap_or(2.0),
                },
                "highpass" => camillalib::config::DitherParameters::Highpass { bits },
                "lipshitz441" => camillalib::config::DitherParameters::Lipshitz441 { bits },
                _ => panic!("unsupported dither type in harness"),
            };
            let mut filter = camillalib::filters::dither::Dither::from_config("test", conf);
            let input = read_f64(in_path);
            let output = run_filter(&mut filter, input, chunk_size);
            write_f64(out_path, &output);
        }
        "clipper" => {
            let clip_limit: f64 = args[2].parse().unwrap();
            let soft_clip: bool = args[3] == "1";
            let chunk_size: usize = args[4].parse().unwrap();
            let in_path = &args[5];
            let out_path = &args[6];
            let conf = camillalib::config::ClipperParameters {
                clip_limit,
                soft_clip: Some(soft_clip),
            };
            let mut filter = camillalib::filters::clipper::Clipper::from_config("test", conf);
            let input = read_f64(in_path);
            let output = run_filter(&mut filter, input, chunk_size);
            write_f64(out_path, &output);
        }
        "lookahead_limiter" => {
            let limit: f64 = args[2].parse().unwrap();
            let attack: f64 = args[3].parse().unwrap();
            let release: f64 = args[4].parse().unwrap();
            let unit_str = &args[5];
            let samplerate: usize = args[6].parse().unwrap();
            let chunk_size: usize = args[7].parse().unwrap();
            let in_path = &args[8];
            let out_path = &args[9];
            let unit = match unit_str.as_str() {
                "ms" => camillalib::config::TimeUnit::Milliseconds,
                "us" => camillalib::config::TimeUnit::Microseconds,
                "s" => camillalib::config::TimeUnit::Seconds,
                "samples" => camillalib::config::TimeUnit::Samples,
                _ => panic!("invalid unit"),
            };
            let conf = camillalib::config::LookaheadLimiterParameters {
                limit,
                attack,
                attack_unit: unit,
                release,
                release_unit: unit,
            };
            let mut filter = camillalib::filters::lookahead_limiter::LookaheadLimiter::from_config(
                "test", conf, samplerate, chunk_size,
            );
            let input = read_f64(in_path);
            let output = run_filter(&mut filter, input, chunk_size);
            write_f64(out_path, &output);
        }
        "compressor" => {
            assert!(args.len() == 13, "compressor needs 11 trailing args");
            let attack: f64 = args[2].parse().unwrap();
            let release: f64 = args[3].parse().unwrap();
            let threshold: f64 = args[4].parse().unwrap();
            let factor: f64 = args[5].parse().unwrap();
            let makeup_gain: f64 = args[6].parse().unwrap();
            let soft_clip: bool = args[7] == "1";
            let clip_limit = if args[8] == "none" {
                None
            } else {
                Some(args[8].parse().unwrap())
            };
            let samplerate: usize = args[9].parse().unwrap();
            let chunk_size: usize = args[10].parse().unwrap();
            let in_path = &args[11];
            let out_path = &args[12];

            let params = camillalib::config::CompressorParameters {
                channels: 1,
                monitor_channels: None,
                process_channels: None,
                attack,
                attack_unit: camillalib::config::TimeUnit::Seconds,
                release,
                release_unit: camillalib::config::TimeUnit::Seconds,
                threshold,
                factor,
                makeup_gain: Some(makeup_gain),
                soft_clip: Some(soft_clip),
                clip_limit,
            };
            let mut processor = Compressor::from_config("test", params, samplerate, chunk_size);
            let input = read_f64(in_path);

            let mut output = Vec::with_capacity(input.len());
            for chunk in input.chunks(chunk_size) {
                let waveforms = vec![chunk.to_vec()];
                let mut audio_chunk =
                    AudioChunk::new(waveforms, 1.0, -1.0, chunk.len(), chunk.len());
                processor
                    .process_chunk(&mut audio_chunk)
                    .expect("process_chunk");
                output.extend_from_slice(&audio_chunk.waveforms[0]);
            }
            write_f64(out_path, &output);
        }
        "noisegate" => {
            assert!(args.len() == 10, "noisegate needs 8 trailing args");
            let attack: f64 = args[2].parse().unwrap();
            let release: f64 = args[3].parse().unwrap();
            let threshold: f64 = args[4].parse().unwrap();
            let attenuation: f64 = args[5].parse().unwrap();
            let samplerate: usize = args[6].parse().unwrap();
            let chunk_size: usize = args[7].parse().unwrap();
            let in_path = &args[8];
            let out_path = &args[9];

            let params = camillalib::config::NoiseGateParameters {
                channels: 1,
                monitor_channels: None,
                process_channels: None,
                attack,
                attack_unit: camillalib::config::TimeUnit::Seconds,
                release,
                release_unit: camillalib::config::TimeUnit::Seconds,
                threshold,
                attenuation,
            };
            let mut processor = NoiseGate::from_config("test", params, samplerate, chunk_size);
            let input = read_f64(in_path);

            let mut output = Vec::with_capacity(input.len());
            for chunk in input.chunks(chunk_size) {
                let waveforms = vec![chunk.to_vec()];
                let mut audio_chunk =
                    AudioChunk::new(waveforms, 1.0, -1.0, chunk.len(), chunk.len());
                processor
                    .process_chunk(&mut audio_chunk)
                    .expect("process_chunk");
                output.extend_from_slice(&audio_chunk.waveforms[0]);
            }
            write_f64(out_path, &output);
        }
        "race" => {
            assert!(args.len() == 14, "race needs 12 trailing args");
            let channel_a: usize = args[2].parse().unwrap();
            let channel_b: usize = args[3].parse().unwrap();
            let delay: f64 = args[4].parse().unwrap();
            let unit_str = &args[5];
            let subsample_delay: bool = args[6] == "1";
            let attenuation: f64 = args[7].parse().unwrap();
            let samplerate: usize = args[8].parse().unwrap();
            let chunk_size: usize = args[9].parse().unwrap();
            let in_ch0 = &args[10];
            let in_ch1 = &args[11];
            let out_ch0 = &args[12];
            let out_ch1 = &args[13];

            let delay_unit = match unit_str.as_str() {
                "ms" => camillalib::config::DelayUnit::Milliseconds,
                "us" => camillalib::config::DelayUnit::Microseconds,
                "s" => camillalib::config::DelayUnit::Seconds,
                "samples" => camillalib::config::DelayUnit::Samples,
                "mm" => camillalib::config::DelayUnit::Millimetres,
                _ => panic!("invalid unit"),
            };

            let params = camillalib::config::RACEParameters {
                channels: 2,
                channel_a,
                channel_b,
                delay,
                subsample_delay: Some(subsample_delay),
                delay_unit,
                attenuation,
            };
            let mut processor = RACE::from_config("test", params, samplerate);
            let input0 = read_f64(in_ch0);
            let input1 = read_f64(in_ch1);
            assert!(
                input0.len() == input1.len(),
                "input channel lengths must match"
            );

            let mut output0 = Vec::with_capacity(input0.len());
            let mut output1 = Vec::with_capacity(input1.len());

            for (chunk0, chunk1) in input0.chunks(chunk_size).zip(input1.chunks(chunk_size)) {
                let waveforms = vec![chunk0.to_vec(), chunk1.to_vec()];
                let mut audio_chunk =
                    AudioChunk::new(waveforms, 1.0, -1.0, chunk0.len(), chunk0.len());
                processor
                    .process_chunk(&mut audio_chunk)
                    .expect("process_chunk");
                output0.extend_from_slice(&audio_chunk.waveforms[0]);
                output1.extend_from_slice(&audio_chunk.waveforms[1]);
            }
            write_f64(out_ch0, &output0);
            write_f64(out_ch1, &output1);
        }
        "lookahead_limiter_proc" => {
            assert!(args.len() == 16, "lookahead_limiter_proc needs 14 trailing args");
            let channels: usize = args[2].parse().unwrap();
            let monitor_str = &args[3];
            let process_str = &args[4];
            let limit: f64 = args[5].parse().unwrap();
            let attack: f64 = args[6].parse().unwrap();
            let release: f64 = args[7].parse().unwrap();
            let unit_str = &args[8];
            let delay_processed_only: bool = args[9] == "1";
            let samplerate: usize = args[10].parse().unwrap();
            let chunk_size: usize = args[11].parse().unwrap();
            let in_ch0 = &args[12];
            let in_ch1 = &args[13];
            let out_ch0 = &args[14];
            let out_ch1 = &args[15];

            let monitor_channels = if monitor_str == "none" {
                None
            } else {
                Some(monitor_str.split(',').map(|s| s.parse().unwrap()).collect())
            };

            let process_channels = if process_str == "none" {
                None
            } else {
                Some(process_str.split(',').map(|s| s.parse().unwrap()).collect())
            };

            let unit = match unit_str.as_str() {
                "ms" => camillalib::config::TimeUnit::Milliseconds,
                "us" => camillalib::config::TimeUnit::Microseconds,
                "s" => camillalib::config::TimeUnit::Seconds,
                "samples" => camillalib::config::TimeUnit::Samples,
                _ => panic!("invalid unit"),
            };

            let params = camillalib::config::LookaheadLimiterProcessorParameters {
                channels,
                monitor_channels,
                process_channels,
                limit,
                attack,
                attack_unit: unit,
                release,
                release_unit: unit,
                delay_processed_only: Some(delay_processed_only),
            };

            let mut processor = camillalib::processors::lookahead_limiter::LookaheadLimiter::from_config(
                "test", params, samplerate, chunk_size,
            );

            let input0 = read_f64(in_ch0);
            let input1 = read_f64(in_ch1);
            assert!(
                input0.len() == input1.len(),
                "input channel lengths must match"
            );

            let mut output0 = Vec::with_capacity(input0.len());
            let mut output1 = Vec::with_capacity(input1.len());

            for (chunk0, chunk1) in input0.chunks(chunk_size).zip(input1.chunks(chunk_size)) {
                let waveforms = vec![chunk0.to_vec(), chunk1.to_vec()];
                let mut audio_chunk =
                    AudioChunk::new(waveforms, 1.0, -1.0, chunk0.len(), chunk0.len());
                processor
                    .process_chunk(&mut audio_chunk)
                    .expect("process_chunk");
                output0.extend_from_slice(&audio_chunk.waveforms[0]);
                output1.extend_from_slice(&audio_chunk.waveforms[1]);
            }
            write_f64(out_ch0, &output0);
            write_f64(out_ch1, &output1);
        }
        other => {
            eprintln!("unknown mode: {other}");
            std::process::exit(2);
        }
    }
}
