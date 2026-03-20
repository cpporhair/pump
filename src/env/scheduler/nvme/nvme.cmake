find_package(PkgConfig)

pkg_check_modules(SPDK REQUIRED spdk_nvme spdk_env_dpdk spdk_syslibs spdk_vmd)


include_directories(${SPDK_INCLUDE_DIRS})
link_directories(${SPDK_LIBRARY_DIRS})

