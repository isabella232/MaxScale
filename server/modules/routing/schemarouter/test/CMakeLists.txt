if(MYSQLCLIENT_FOUND AND BUILD_TESTS)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/test.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/test.cmake @ONLY)
  add_executable(testschemarouter testschemarouter.c)
  target_link_libraries(testschemarouter ${MYSQLCLIENT_LIBRARIES} ssl crypto dl z m rt pthread)
  add_executable(testschemarouter2 testschemarouter2.c)
  target_link_libraries(testschemarouter2 ${MYSQLCLIENT_LIBRARIES} ssl crypto dl z m rt pthread)
  add_test(NAME TestSchemaRouter COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/test.cmake)

endif()
