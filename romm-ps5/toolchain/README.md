# Toolchain setup

RomM-PS5 cross-compiles with the [ps5-payload-dev](https://github.com/ps5-payload-dev)
SDK. These exact steps were run successfully during Phase 1 research (see
`docs/architecture.md` Section 7) on a plain Linux x86_64 host with `clang-18`
and `lld-18` already present — no Docker required.

## 1. Fetch the SDK

```sh
curl -fsSL -o ps5-payload-sdk.zip \
    https://github.com/ps5-payload-dev/sdk/releases/latest/download/ps5-payload-sdk.zip
sudo unzip -q -d /opt ps5-payload-sdk.zip
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
```

Do **not** commit `ps5-payload-sdk.zip` or the unpacked SDK to this
repository — it's a third-party download, fetched fresh by CI and by
contributors, not a vendored blob.

## 2. Configure and build

```sh
cd romm-ps5
cmake -B build -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake"
cmake --build build
```

This produces `build/romm-ps5.elf`. It has **not** been run on a PS5 as part
of this repository's development so far — see `docs/architecture.md` for
exactly what has and hasn't been verified.

## 3. Deploying to a console (once there's something worth deploying)

The SDK ships `prospero-deploy` (also exposed as the CMake `PS5_DEPLOY`
variable) for pushing an ELF to a jailbroken PS5's ELF loader over the
network:

```sh
$PS5_PAYLOAD_SDK/bin/prospero-deploy -h <ps5-ip> -p 9021 build/romm-ps5.elf
```

This requires a jailbroken PS5 on the same network running an ELF loader
(e.g. `ps5-payload-dev/elfldr`) — nothing in this repository does that setup
for you, and none of the RomM-PS5 authors have run this against real hardware
yet.
