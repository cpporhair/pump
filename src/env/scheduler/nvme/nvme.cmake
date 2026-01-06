find_package(PkgConfig)

set(ENV{PKG_CONFIG_PATH} /home/null/work/kv/spdk/build/lib/pkgconfig)

pkg_check_modules(SPDK REQUIRED spdk_nvme spdk_env_dpdk spdk_syslibs spdk_vmd)


include_directories(${SPDK_INCLUDE_DIRS})
link_directories(${SPDK_LIBRARY_DIRS})

