subinclude(
    "///cc//build_defs:c",
    "///go//build_defs:go",
)

c_binary(
    name = "preamble",
    srcs = ["preamble.c"],
    compiler_flags = [
        "-std=c11",
        "-D_POSIX_C_SOURCE=200809L",
    ],
    static = True,
    visibility = [
        "//tools/please_pex/pex:preamble",
    ],
    deps = [
        "///third_party/cc/cjson/cjson//:cjson",
        "///third_party/cc/kuba_zip/kuba_zip//:kuba_zip",
        "///third_party/cc/log_c/log_c//:log_c",
    ],
)

go_library(
    name = "preamble_go",
    srcs = glob(["*.go"]),
    visibility = [
        "//tools/please_pex/pex",
    ],
)
