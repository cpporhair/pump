
add_executable(example.pump ./example/hello_world/pump.cc)
add_executable(example.echo ./example/echo/echo.cc)
add_executable(example.echo_client ./example/echo_client/echo_client.cc)
add_executable(example.share_nothing ./example/share_nothing/main.cc)
add_executable(example.nvme ./example/nvme/nvme_test.cc)
add_executable(example.coro ./example/coro/main.cc)
add_executable(example.concurrent ./example/concurrent/main.cc)
add_executable(example.rpc_echo_server ./example/rpc_echo/server.cc)
add_executable(example.rpc_echo_client ./example/rpc_echo/client.cc)
add_executable(apps.aisaq ./aisaq/main.cc)
add_executable(apps.kv ./kv/main.cc)
add_executable(test.sequential ./test/sequential_test.cc)
target_link_libraries(test.sequential PRIVATE pthread numa)
add_executable(test.when_any ./test/when_any_test.cc)
target_link_libraries(test.when_any PRIVATE pthread numa)

add_executable(test.rpc_timeout ./test/rpc_timeout_test.cc)
target_link_libraries(test.rpc_timeout PRIVATE ${URING_LIBRARY})

add_executable(test.rpc_concurrent ./test/rpc_concurrent_test.cc)
target_link_libraries(test.rpc_concurrent PRIVATE ${URING_LIBRARY})

add_executable(test.rpc_disconnect ./test/rpc_disconnect_test.cc)
target_link_libraries(test.rpc_disconnect PRIVATE ${URING_LIBRARY})

target_link_libraries(example.echo PRIVATE ${URING_LIBRARY})
target_link_libraries(example.echo_client PRIVATE ${URING_LIBRARY})
target_link_libraries(example.rpc_echo_server PRIVATE ${URING_LIBRARY})
target_link_libraries(example.rpc_echo_client PRIVATE ${URING_LIBRARY})
target_link_libraries(example.share_nothing PRIVATE numa)
target_link_libraries(example.pump PRIVATE numa)
target_link_libraries(example.nvme PRIVATE 
        -Wl,--whole-archive ${SPDK_LIBRARIES} -Wl,--no-whole-archive
        pthread isal ssl rt uuid crypto m aio numa dl)
target_link_libraries(apps.aisaq PRIVATE
        -Wl,--whole-archive ${SPDK_LIBRARIES} -Wl,--no-whole-archive
        pthread isal ssl rt uuid crypto m aio numa dl)
target_link_libraries(apps.kv PRIVATE
        -Wl,--whole-archive ${SPDK_LIBRARIES} -Wl,--no-whole-archive
        pthread isal ssl rt uuid crypto m aio numa dl)

#target_link_libraries (example.kv
#        -ltbb
#        -pg
#        -Wl,--whole-archive  ${SPDK_LIBRARIES} -Wl,--no-whole-archive
#        boost_program_options pthread isal ssl rt uuid crypto m aio numa dl  )

