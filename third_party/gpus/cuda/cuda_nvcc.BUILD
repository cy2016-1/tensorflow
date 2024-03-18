licenses(["restricted"])  # NVIDIA proprietary license

exports_files([
    "version.txt",
    "bin/nvcc",
])

filegroup(
    name = "include",
    srcs = glob([
        "include/**",
    ]),
)

filegroup(
    name = "nvvm",
    srcs = [
        "nvvm/libdevice/libdevice.10.bc",
    ],
    visibility = ["//visibility:public"],
)

filegroup(
    name = "bin",
    srcs = glob([
        "bin/**",
        "nvvm/bin/**",
    ]),
    visibility = ["//visibility:public"],
)

filegroup(
    name = "nvcc",
    srcs = [
        "bin/nvcc",
    ],
    visibility = ["@local_config_cuda//cuda:__pkg__"],
)

filegroup(
    name = "nvlink",
    srcs = [
        "bin/nvlink",
    ],
    visibility = ["//visibility:public"],
)

filegroup(
    name = "fatbinary",
    srcs = [
        "bin/fatbinary",
    ],
    visibility = ["//visibility:public"],
)

filegroup(
    name = "bin2c",
    srcs = [
        "bin/bin2c",
    ],
    visibility = ["//visibility:public"],
)

filegroup(
    name = "link_stub",
    srcs = [
        "bin/crt/link.stub",
    ],
    visibility = ["//visibility:public"],
)

cc_library(
    name = "headers",
    hdrs = [":include"],
    include_prefix = "third_party/gpus/cuda/include",
    includes = ["include"],
    strip_include_prefix = "include",
    visibility = ["@local_config_cuda//cuda:__pkg__"],
)
