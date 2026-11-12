add_library(simplestream
MODULE
  simplestream.cc
)

target_link_libraries(simplestream trunk_recorder_library ssl crypto ${CURL_LIBRARIES} ${Boost_LIBRARIES} ${GNURADIO_PMT_LIBRARIES} ${GNURADIO_RUNTIME_LIBRARIES} ${GNURADIO_FILTER_LIBRARIES} ${GNURADIO_DIGITAL_LIBRARIES} ${GNURADIO_ANALOG_LIBRARIES} ${GNURADIO_AUDIO_LIBRARIES} ${GNURADIO_UHD_LIBRARIES} ${UHD_LIBRARIES} ${GNURADIO_BLOCKS_LIBRARIES} ${GNURADIO_OSMOSDR_LIBRARIES}  ${LIBOP25_REPEATER_LIBRARIES} gnuradio-op25_repeater) # gRPC::grpc++_reflection protobuf::libprotobuf)

if(NOT Gnuradio_VERSION VERSION_LESS "3.8")

    target_link_libraries(simplestream
    gnuradio::gnuradio-analog
    gnuradio::gnuradio-blocks
    gnuradio::gnuradio-digital
    gnuradio::gnuradio-filter
    gnuradio::gnuradio-pmt
    )  
    
endif()

install(TARGETS simplestream LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/trunk-recorder)
option(SIMPLESTREAM_TESTS "Build the simplestream offline wire test" OFF)
if(SIMPLESTREAM_TESTS)
  add_executable(simplestream_test simplestream_test.cc)
  get_target_property(simplestream_libs simplestream LINK_LIBRARIES)
  target_link_libraries(simplestream_test ${simplestream_libs})
  if (GR_IIO_FOUND)
    target_link_libraries(simplestream_test gnuradio::gnuradio-iio)
  endif()
endif()
