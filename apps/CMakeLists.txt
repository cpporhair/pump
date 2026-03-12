
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
add_executable(test.scope_slab ./test/scope_slab_test.cc)

target_link_libraries(test.sequential PRIVATE pthread numa)
target_link_libraries(test.when_any PRIVATE pthread numa)
target_link_libraries(test.scope_stress PRIVATE pthread numa)
target_link_libraries(test.scope_slab PRIVATE pthread numa)
add_executable(test.concurrent_loop ./test/concurrent_loop_test.cc)
target_link_libraries(test.concurrent_loop PRIVATE pthread numa)
target_link_libraries(example.rpc PRIVATE ${URING_LIBRARY})
target_link_libraries(example.echo PRIVATE ${URING_LIBRARY} pthread)
add_executable(example.dpdk ./example/dpdk/main.cc)
target_link_libraries(example.dpdk PRIVATE ${DPDK_LIBRARIES} pthread)

# CUDA example: compile kernel to PTX, link host code with CUDA Driver API
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/vector_add_kernel.ptx
    COMMAND ${CUDAToolkit_NVCC_EXECUTABLE} --ptx
        ${CMAKE_CURRENT_SOURCE_DIR}/example/cuda/vector_add_kernel.cu
        -o ${CMAKE_CURRENT_BINARY_DIR}/vector_add_kernel.ptx
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/example/cuda/vector_add_kernel.cu
)
add_custom_target(cuda_ptx DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/vector_add_kernel.ptx)
add_executable(example.cuda ./example/cuda/main.cc)
add_dependencies(example.cuda cuda_ptx)
target_link_libraries(example.cuda PRIVATE CUDA::cuda_driver)
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

