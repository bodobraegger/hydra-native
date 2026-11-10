# Builds webrtc-audio-processing 1.3 from source as the static library `webrtc-audio-processing`, for systems
# without the library of PipeWire (macOS, Windows). The lists follow the meson.build files of the release
# (webrtc/rtc_base, api, system_wrappers, common_audio, third_party/pffft, third_party/rnnoise, the VAD of
# modules/audio_coding, and modules/audio_processing). Abseil is used only through its headers: with C++17,
# absl::optional and absl::string_view are the types of the standard library.

set(WEBRTC_SOURCE ${VENDOR}/webrtc-audio-processing/webrtc)

set(WEBRTC_SOURCES
  rtc_base/checks.cc rtc_base/event.cc rtc_base/event_tracer.cc rtc_base/experiments/field_trial_parser.cc
  rtc_base/logging.cc rtc_base/memory/aligned_malloc.cc rtc_base/platform_thread.cc
  rtc_base/platform_thread_types.cc rtc_base/race_checker.cc rtc_base/string_encode.cc
  rtc_base/string_to_number.cc rtc_base/string_utils.cc rtc_base/strings/string_builder.cc
  rtc_base/synchronization/mutex.cc rtc_base/synchronization/rw_lock_wrapper.cc rtc_base/synchronization/yield.cc
  rtc_base/synchronization/yield_policy.cc rtc_base/system/file_wrapper.cc rtc_base/time_utils.cc
  rtc_base/zero_memory.cc

  api/audio/audio_frame.cc api/audio/channel_layout.cc api/audio/echo_canceller3_config.cc
  api/audio_codecs/audio_decoder.cc api/audio_codecs/audio_encoder.cc api/rtp_headers.cc api/rtp_packet_info.cc
  api/task_queue/task_queue_base.cc api/units/data_rate.cc api/units/data_size.cc api/units/frequency.cc
  api/units/time_delta.cc api/units/timestamp.cc api/video/color_space.cc api/video/hdr_metadata.cc
  api/video/video_content_type.cc api/video/video_timing.cc

  system_wrappers/source/cpu_features.cc system_wrappers/source/field_trial.cc system_wrappers/source/metrics.cc
  system_wrappers/source/sleep.cc

  third_party/pffft/src/pffft.c third_party/rnnoise/src/rnn_vad_weights.cc

  modules/audio_coding/codecs/isac/main/source/filter_functions.c
  modules/audio_coding/codecs/isac/main/source/isac_vad.c
  modules/audio_coding/codecs/isac/main/source/pitch_estimator.c
  modules/audio_coding/codecs/isac/main/source/pitch_filter.c)

foreach(source audio_converter.cc audio_util.cc channel_buffer.cc fir_filter_c.cc fir_filter_factory.cc
    real_fourier.cc real_fourier_ooura.cc resampler/push_resampler.cc resampler/push_sinc_resampler.cc
    resampler/resampler.cc resampler/sinc_resampler.cc resampler/sinusoidal_linear_chirp_source.cc ring_buffer.c
    smoothing_filter.cc third_party/ooura/fft_size_128/ooura_fft.cc third_party/ooura/fft_size_256/fft4g.cc
    third_party/spl_sqrt_floor/spl_sqrt_floor.c vad/vad.cc vad/vad_core.c vad/vad_filterbank.c vad/vad_gmm.c
    vad/vad_sp.c vad/webrtc_vad.c wav_file.cc wav_header.cc window_generator.cc)
  list(APPEND WEBRTC_SOURCES common_audio/${source})
endforeach()
foreach(source auto_correlation.c auto_corr_to_refl_coef.c complex_bit_reverse.c complex_fft.c
    copy_set_operations.c cross_correlation.c division_operations.c dot_product_with_scale.cc downsample_fast.c
    filter_ar.c filter_ar_fast_q12.c energy.c filter_ma_fast_q12.c get_hanning_window.c get_scaling_square.c
    ilbc_specific_functions.c levinson_durbin.c lpc_to_refl_coef.c min_max_operations.c randomization_functions.c
    real_fft.c refl_coef_to_lpc.c resample_48khz.c resample_by_2.c resample_by_2_internal.c resample.c
    resample_fractional.c spl_init.c spl_inl.c splitting_filter.c spl_sqrt.c sqrt_of_one_minus_x_squared.c
    vector_scaling_operations.c)
  list(APPEND WEBRTC_SOURCES common_audio/signal_processing/${source})
endforeach()

foreach(source aec_dump/null_aec_dump_factory.cc aecm/aecm_core.cc aecm/aecm_core_c.cc aecm/echo_control_mobile.cc
    agc/agc.cc agc/agc_manager_direct.cc agc/legacy/analog_agc.cc agc/legacy/digital_agc.cc
    agc/loudness_histogram.cc agc/utility.cc audio_buffer.cc audio_processing_builder_impl.cc
    audio_processing_impl.cc echo_control_mobile_impl.cc echo_detector/circular_buffer.cc
    echo_detector/mean_variance_estimator.cc echo_detector/moving_max.cc
    echo_detector/normalized_covariance_estimator.cc gain_control_impl.cc gain_controller2.cc high_pass_filter.cc
    include/aec_dump.cc include/audio_frame_proxies.cc include/audio_processing.cc
    include/audio_processing_statistics.cc include/config.cc level_estimator.cc logging/apm_data_dumper.cc
    optionally_built_submodule_creators.cc residual_echo_detector.cc rms_level.cc splitting_filter.cc
    three_band_filter_bank.cc transient/file_utils.cc transient/moving_moments.cc transient/transient_detector.cc
    transient/transient_suppressor_impl.cc transient/wpd_node.cc transient/wpd_tree.cc typing_detection.cc
    utility/cascaded_biquad_filter.cc utility/delay_estimator.cc utility/delay_estimator_wrapper.cc
    utility/pffft_wrapper.cc vad/gmm.cc vad/pitch_based_vad.cc vad/pitch_internal.cc vad/pole_zero_filter.cc
    vad/standalone_vad.cc vad/vad_audio_proc.cc vad/vad_circular_buffer.cc vad/voice_activity_detector.cc
    voice_detection.cc)
  list(APPEND WEBRTC_SOURCES modules/audio_processing/${source})
endforeach()
foreach(source adaptive_fir_filter.cc adaptive_fir_filter_erl.cc aec3_common.cc aec3_fft.cc aec_state.cc
    alignment_mixer.cc api_call_jitter_metrics.cc block_buffer.cc block_delay_buffer.cc block_framer.cc
    block_processor.cc block_processor_metrics.cc clockdrift_detector.cc coarse_filter_update_gain.cc
    comfort_noise_generator.cc decimator.cc dominant_nearend_detector.cc downsampled_render_buffer.cc
    echo_audibility.cc echo_canceller3.cc echo_path_delay_estimator.cc echo_path_variability.cc echo_remover.cc
    echo_remover_metrics.cc erle_estimator.cc erl_estimator.cc fft_buffer.cc filter_analyzer.cc frame_blocker.cc
    fullband_erle_estimator.cc matched_filter.cc matched_filter_lag_aggregator.cc moving_average.cc
    refined_filter_update_gain.cc render_buffer.cc render_delay_buffer.cc render_delay_controller.cc
    render_delay_controller_metrics.cc render_signal_analyzer.cc residual_echo_estimator.cc
    reverb_decay_estimator.cc reverb_frequency_response.cc reverb_model.cc reverb_model_estimator.cc
    signal_dependent_erle_estimator.cc spectrum_buffer.cc stationarity_estimator.cc subband_erle_estimator.cc
    subband_nearend_detector.cc subtractor.cc subtractor_output_analyzer.cc subtractor_output.cc
    suppression_filter.cc suppression_gain.cc transparent_mode.cc)
  list(APPEND WEBRTC_SOURCES modules/audio_processing/aec3/${source})
endforeach()
foreach(source adaptive_agc.cc adaptive_digital_gain_applier.cc adaptive_mode_level_estimator_agc.cc
    adaptive_mode_level_estimator.cc agc2_testing_common.cc biquad_filter.cc compute_interpolated_gain_curve.cc
    down_sampler.cc fixed_digital_level_estimator.cc gain_applier.cc interpolated_gain_curve.cc limiter.cc
    limiter_db_gain_curve.cc noise_level_estimator.cc noise_spectrum_estimator.cc rnn_vad/auto_correlation.cc
    rnn_vad/common.cc rnn_vad/features_extraction.cc rnn_vad/lp_residual.cc rnn_vad/pitch_search.cc
    rnn_vad/pitch_search_internal.cc rnn_vad/rnn.cc rnn_vad/spectral_features.cc
    rnn_vad/spectral_features_internal.cc saturation_protector.cc signal_classifier.cc vad_with_level.cc
    vector_float_frame.cc)
  list(APPEND WEBRTC_SOURCES modules/audio_processing/agc2/${source})
endforeach()
foreach(source fast_math.cc histograms.cc noise_estimator.cc noise_suppressor.cc ns_fft.cc prior_signal_model.cc
    prior_signal_model_estimator.cc quantile_noise_estimator.cc signal_model.cc signal_model_estimator.cc
    speech_probability_estimator.cc suppression_params.cc wiener_filter.cc)
  list(APPEND WEBRTC_SOURCES modules/audio_processing/ns/${source})
endforeach()

if(WIN32)
  list(APPEND WEBRTC_SOURCES rtc_base/synchronization/rw_lock_win.cc)
else()
  list(APPEND WEBRTC_SOURCES rtc_base/synchronization/rw_lock_posix.cc)
endif()

# The vector code of x86-64 and arm64. The library selects AVX2 at run time, after a check of the processor.
set(WEBRTC_AVX2_SOURCES common_audio/fir_filter_avx2.cc common_audio/resampler/sinc_resampler_avx2.cc
  modules/audio_processing/aec3/adaptive_fir_filter_avx2.cc modules/audio_processing/aec3/adaptive_fir_filter_erl_avx2.cc
  modules/audio_processing/aec3/fft_data_avx2.cc modules/audio_processing/aec3/matched_filter_avx2.cc
  modules/audio_processing/aec3/vector_math_avx2.cc)
set(WEBRTC_SSE2_SOURCES common_audio/fir_filter_sse.cc common_audio/resampler/sinc_resampler_sse.cc
  common_audio/third_party/ooura/fft_size_128/ooura_fft_sse2.cc)
set(WEBRTC_NEON_SOURCES common_audio/fir_filter_neon.cc common_audio/resampler/sinc_resampler_neon.cc
  common_audio/signal_processing/cross_correlation_neon.c common_audio/signal_processing/downsample_fast_neon.c
  common_audio/signal_processing/min_max_operations_neon.c common_audio/third_party/ooura/fft_size_128/ooura_fft_neon.cc
  modules/audio_processing/aecm/aecm_core_neon.cc)

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" WEBRTC_PROCESSOR)
if(WEBRTC_PROCESSOR MATCHES "^(x86_64|amd64|x64)$")
  set(WEBRTC_ARCH_DEFINITIONS WEBRTC_ENABLE_AVX2)
  list(APPEND WEBRTC_SOURCES ${WEBRTC_SSE2_SOURCES} ${WEBRTC_AVX2_SOURCES})
  if(MSVC)
    set(WEBRTC_AVX2_FLAGS /arch:AVX2)
  else()
    set(WEBRTC_AVX2_FLAGS -mavx2 -mfma)
  endif()
  list(TRANSFORM WEBRTC_AVX2_SOURCES PREPEND ${WEBRTC_SOURCE}/)
  set_source_files_properties(${WEBRTC_AVX2_SOURCES} PROPERTIES COMPILE_OPTIONS "${WEBRTC_AVX2_FLAGS}")
elseif(WEBRTC_PROCESSOR MATCHES "^(arm64|aarch64)$")
  set(WEBRTC_ARCH_DEFINITIONS WEBRTC_ARCH_ARM64 WEBRTC_HAS_NEON)
  list(APPEND WEBRTC_SOURCES ${WEBRTC_NEON_SOURCES})
else()
  message(FATAL_ERROR "webrtc-audio-processing: no build for the processor ${CMAKE_SYSTEM_PROCESSOR}")
endif()

list(TRANSFORM WEBRTC_SOURCES PREPEND ${WEBRTC_SOURCE}/)
add_library(webrtc-audio-processing STATIC ${WEBRTC_SOURCES})
set_target_properties(webrtc-audio-processing PROPERTIES CXX_STANDARD 17 POSITION_INDEPENDENT_CODE ON)
target_include_directories(webrtc-audio-processing PUBLIC ${WEBRTC_SOURCE} ${VENDOR}/abseil-cpp)
target_compile_definitions(webrtc-audio-processing PRIVATE WEBRTC_LIBRARY_IMPL WEBRTC_APM_DEBUG_DUMP=0 NDEBUG
  ${WEBRTC_ARCH_DEFINITIONS})
if(WIN32)
  target_compile_definitions(webrtc-audio-processing PUBLIC WEBRTC_WIN NOMINMAX _USE_MATH_DEFINES
    __STDC_FORMAT_MACROS=1)
  target_link_libraries(webrtc-audio-processing PUBLIC winmm)
elseif(APPLE)
  target_compile_definitions(webrtc-audio-processing PUBLIC WEBRTC_MAC WEBRTC_POSIX)
  target_link_libraries(webrtc-audio-processing PUBLIC "-framework Foundation")
else()
  target_compile_definitions(webrtc-audio-processing PUBLIC WEBRTC_LINUX WEBRTC_POSIX WEBRTC_THREAD_RR)
  find_package(Threads REQUIRED)
  target_link_libraries(webrtc-audio-processing PUBLIC Threads::Threads)
endif()
# Some headers of the release use uint32_t without <cstdint>, which newer standard libraries no longer include
# through other headers.
if(MSVC)
  target_compile_options(webrtc-audio-processing PRIVATE ${QUIET} $<$<COMPILE_LANGUAGE:CXX>:/FIcstdint>)
else()
  target_compile_options(webrtc-audio-processing PRIVATE ${QUIET} "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cstdint>")
  set_source_files_properties(${WEBRTC_SOURCE}/third_party/pffft/src/pffft.c PROPERTIES COMPILE_DEFINITIONS _GNU_SOURCE)
endif()
