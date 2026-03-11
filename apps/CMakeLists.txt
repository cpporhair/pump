
add_executable(example.pump ./example/hello_world/pump.cc)
add_executable(example.echo ./example/echo/main.cc)
add_executable(example.rpc ./example/rpc/rpc.cc)
add_executable(example.share_nothing ./example/share_nothing/main.cc)
add_executable(example.nvme ./example/nvme/nvme_test.cc)
add_executable(example.coro ./example/coro/main.cc)
add_executable(example.concurrent ./example/concurrent/main.cc)
add_executable(apps.aisaq ./aisaq/main.cc)
add_executable(apps.kv ./kv/main.cc)
add_executable(test.sequential ./test/sequential_test.cc)
add_executable(test.when_any ./test/when_any_test.cc)
add_executable(test.scope_stress ./test/scope_stress_test.cc)

target_link_libraries(test.sequential PRIVATE pthread numa)
target_link_libraries(test.when_any PRIVATE pthread numa)
target_link_libraries(test.scope_stress PRIVATE pthread numa)
target_link_libraries(example.rpc PRIVATE ${URING_LIBRARY})
target_link_libraries(example.echo PRIVATE ${URING_LIBRARY} pthread)
add_executable(example.dpdk ./example/dpdk/main.cc)
target_link_libraries(example.dpdk PRIVATE ${DPDK_LIBRARIES} pthread)
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

