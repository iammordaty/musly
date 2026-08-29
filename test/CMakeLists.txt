# CMake buildfile generator file.
# Process with cmake to create your desired buildfiles.

# (c) 2014, Jan Schlueter <jan.schlueter@ofai.at>

include_directories(
    "${PROJECT_SOURCE_DIR}/libmusly"
    "${PROJECT_SOURCE_DIR}/musly")

add_executable(selftest
    "${PROJECT_SOURCE_DIR}/musly/tools.cpp"
    main.cpp)

target_link_libraries(selftest
    libmusly)

add_test(NAME selftest COMMAND selftest)

# Diagnostic tool, not a pass/fail test: it reports numbers to interpret and
# takes far too long for the regular test run. Deliberately not registered
# with add_test(); run it by hand when touching the reference set policy.
add_executable(reference_set_experiment
    reference_set_experiment.cpp)

target_link_libraries(reference_set_experiment
    libmusly)

# Timing harness for jukebox build / load / query. Not a pass/fail test.
add_executable(jukebox_baseline
    jukebox_baseline.cpp)

target_link_libraries(jukebox_baseline
    libmusly)
