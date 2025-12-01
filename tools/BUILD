_PLEASE_PEX_VERSION = "2.2.0"

remote_file(
    name = "please_pex",
    url = f"https://github.com/please-build/python-rules/releases/download/please_pex-v{_PLEASE_PEX_VERSION}/please_pex-{_PLEASE_PEX_VERSION}-{CONFIG.OS}_{CONFIG.ARCH}",
    hashes = [
        "692069ea340d6ea741062e7ea1da4fed454ec761e397bde1f2acfcb9f2b18756",  # darwin_amd64
        "75299030c146fcf50e406cbf7746cd1e678340d438c4e4a3bd74a47148435dbc",  # darwin_arm64
        "04d52d00a06b21805f20fa685a551e4979ff7b30be331a049152c24466696d77",  # freebsd_amd64
        "c324ab1b51b0cf0bc65e01cd1713d28d5a11ea6c591ce553b6b2c13ddcbc84ea",  # linux_amd64
        "f5b03d4a0b0de3765ae5f656386dd6a681831dec6284b356ec2ea671d2d3b271",  # linux_arm64
    ],
    binary = True,
    visibility = ["PUBLIC"],
)
